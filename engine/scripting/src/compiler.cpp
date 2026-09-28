#include "scripting/compiler.hpp"

#include "app/app.hpp"
#include "asset/handle.hpp"
#include "compiler/compilation_session.hpp"
#include "compiler/module_ir.hpp"
#include "compiler/pass.hpp"
#include "compiler/source_name.hpp"
#include "compiler/state_lowering.hpp"
#include "ecs/dynamic/state.hpp"
#include "ecs/dynamic/system_decl.hpp"
#include "ecs/fwd.hpp"
#include "refl/enum.hpp"
#include "scripting/detail/exported_type.hpp"
#include "scripting/library.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <Luau/Ast.h>
#include <Luau/Common.h>
#include <Luau/Compiler.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ets {
namespace {

using detail::luau_compiler::FunctionIR;
using detail::luau_compiler::module_name_from_source;
using detail::luau_compiler::ModuleIR;
using detail::luau_compiler::PluginDependencyIR;
using detail::luau_compiler::PluginIR;
using detail::luau_compiler::RecordTypeIR;
using detail::luau_compiler::StringUnionTypeIR;
using detail::luau_compiler::TypeDeclIR;
using detail::luau_compiler::UnsupportedTypeIR;

bool enable_luau_language_features() {
    bool found = false;
    for (auto* flag = Luau::FValue<bool>::list; flag != nullptr;
         flag = flag->next) {
        if (std::strcmp(flag->name, "LuauExportValueSyntax") == 0) {
            flag->value = true;
            found = true;
        }
    }
    return found;
}

using Luau::AstExpr;
using Luau::AstExprCall;
using Luau::AstExprConstantBool;
using Luau::AstExprConstantNil;
using Luau::AstExprConstantNumber;
using Luau::AstExprConstantString;
using Luau::AstExprFunction;
using Luau::AstExprGlobal;
using Luau::AstExprIndexExpr;
using Luau::AstExprIndexName;
using Luau::AstExprLocal;
using Luau::AstExprTable;
using Luau::AstLocal;
using Luau::AstStat;
using Luau::AstStatAssign;
using Luau::AstStatCompoundAssign;
using Luau::AstStatExpr;
using Luau::AstStatForIn;
using Luau::AstStatFunction;
using Luau::AstStatLocal;
using Luau::AstStatLocalFunction;
using Luau::AstStatReturn;
using Luau::AstStatTypeAlias;
using Luau::AstType;
using Luau::AstTypeOptional;
using Luau::AstTypeReference;
using Luau::AstTypeSingletonString;
using Luau::AstTypeTable;
using Luau::AstTypeUnion;

std::string_view name_view(Luau::AstName name);

class LuauImportVisitor final : public Luau::AstVisitor {
  public:
    std::vector<std::string> imports;
    Optional<LuauScriptError> error;

    bool visit(AstExprCall* expression) override {
        const auto* global = expression->func->as<AstExprGlobal>();
        if (global == nullptr || global->name.value == nullptr ||
            std::string_view {global->name.value} != "require") {
            return true;
        }
        if (expression->args.size != 1) {
            error = LuauScriptError {
                "Invalid Luau import at line " +
                    std::to_string(expression->location.begin.line + 1) +
                    ": require expects exactly one string literal",
            };
            return false;
        }
        const auto* specifier =
            expression->args.data[0]->as<AstExprConstantString>();
        if (specifier == nullptr) {
            error = LuauScriptError {
                "Invalid Luau import at line " +
                    std::to_string(expression->location.begin.line + 1) +
                    ": require path must be a string literal",
            };
            return false;
        }
        imports.emplace_back(specifier->value.data, specifier->value.size);
        return true;
    }
};

Result<std::vector<std::string>, LuauScriptError>
extract_parsed_luau_script_imports(Luau::AstStatBlock& root) {
    LuauImportVisitor visitor;
    root.visit(&visitor);
    if (visitor.error) {
        return failure(std::move(*visitor.error));
    }
    return std::move(visitor.imports);
}

const AstExpr* assignment_root(const AstExpr& expression) {
    if (const auto* index = expression.as<AstExprIndexName>()) {
        return assignment_root(*index->expr);
    }
    if (const auto* index = expression.as<AstExprIndexExpr>()) {
        return assignment_root(*index->expr);
    }
    return &expression;
}

class LuauSnapshotSafetyVisitor final : public Luau::AstVisitor {
  public:
    Optional<LuauScriptError> error;
    std::string source_name;

    LuauSnapshotSafetyVisitor(
        const Luau::AstStatBlock& root,
        std::string source
    ) : source_name(std::move(source)) {
        for (const AstStat* statement : root.body) {
            if (const auto* local = statement->as<AstStatLocal>()) {
                for (const AstLocal* variable : local->vars) {
                    m_readonly_module_bindings.insert(variable);
                }
                continue;
            }
            if (const auto* function = statement->as<AstStatLocalFunction>()) {
                m_readonly_module_bindings.insert(function->name);
            }
        }
    }

    bool visit(AstStatAssign* statement) override {
        for (AstExpr* target : statement->vars) {
            if (!validate_assignment(*target)) {
                return false;
            }
        }
        const auto count =
            std::min(statement->vars.size, statement->values.size);
        for (std::size_t index = 0; index < count; ++index) {
            update_alias(
                *statement->vars.data[index],
                *statement->values.data[index]
            );
        }
        return true;
    }

    bool visit(AstStatLocal* statement) override {
        const auto count =
            std::min(statement->vars.size, statement->values.size);
        for (std::size_t index = 0; index < count; ++index) {
            if (is_native_module_require(*statement->values.data[index])) {
                m_native_module_imports.insert(statement->vars.data[index]);
                continue;
            }
            if (is_module_require(*statement->values.data[index])) {
                m_module_imports.insert(statement->vars.data[index]);
            }
            if (references_module_state(*statement->values.data[index])) {
                m_module_state_aliases.insert(statement->vars.data[index]);
            }
        }
        return true;
    }

    bool visit(AstStatCompoundAssign* statement) override {
        return validate_assignment(*statement->var);
    }

    bool visit(AstStatFunction* statement) override {
        return validate_assignment(*statement->name);
    }

    bool visit(AstExprIndexName* expression) override {
        std::vector<std::string_view> path;
        if (!append_path(*expression, path)) {
            return true;
        }
        if ((path.size() == 2 && path[0] == "math" &&
             (path[1] == "random" || path[1] == "randomseed")) ||
            (path.size() == 2 && path[0] == "os" &&
             (path[1] == "clock" || path[1] == "time" || path[1] == "date" ||
              path[1] == "difftime"))) {
            reject(
                expression->location,
                "nondeterministic API '" + join_path(path) +
                    "' is unavailable; keep deterministic state in ECS"
            );
            return false;
        }
        return true;
    }

    bool visit(AstExprCall* expression) override {
        bool plugin_system_registration = false;
        if (expression->self) {
            const auto* method = expression->func->as<AstExprIndexName>();
            if (method != nullptr) {
                const auto method_name = name_view(method->index);
                plugin_system_registration =
                    method_name == "add_system" || method_name == "add_systems";
            }
            if (method != nullptr && references_module_state(*method->expr)) {
                reject(
                    expression->location,
                    "method calls cannot mutate captured module state"
                );
                return false;
            }
            if (plugin_system_registration) {
                return false;
            }
        }
        const bool imported_type_call =
            accepts_imported_type_token(*expression);

        std::vector<std::string_view> path;
        static const std::unordered_set<std::string_view> mutating_calls {
            "table.clear",
            "table.insert",
            "table.move",
            "table.remove",
            "table.sort",
            "rawset",
        };
        if (append_path(*expression->func, path)) {
            const auto name = join_path(path);
            if (mutating_calls.contains(name) && expression->args.size > 0 &&
                references_module_state(*expression->args.data[0])) {
                reject(
                    expression->location,
                    "call to '" + name + "' cannot mutate captured module state"
                );
                return false;
            }
        }
        for (AstExpr* argument : expression->args) {
            if (references_module_state(*argument) &&
                !(imported_type_call && is_direct_module_export(*argument))) {
                reject(
                    expression->location,
                    "captured module state cannot be passed to a call"
                );
                return false;
            }
        }
        return true;
    }

    bool visit(Luau::AstExprGlobal* expression) override {
        const auto name = name_view(expression->name);
        if (name == "_G" || name == "getfenv" || name == "setfenv" ||
            name == "tick" || name == "time" || name == "elapsedTime") {
            reject(
                expression->location,
                "snapshot-unsafe global '" + std::string(name) +
                    "' is unavailable"
            );
            return false;
        }
        return true;
    }

  private:
    std::unordered_set<const AstLocal*> m_module_state_aliases;
    std::unordered_set<const AstLocal*> m_module_imports;
    std::unordered_set<const AstLocal*> m_native_module_imports;
    std::unordered_set<const AstLocal*> m_readonly_module_bindings;

    static const AstExprConstantString*
    module_require_specifier(const AstExpr& expression) {
        const auto* call = expression.as<AstExprCall>();
        if (call == nullptr || call->args.size != 1) {
            return nullptr;
        }
        const auto* global = call->func->as<AstExprGlobal>();
        const auto* specifier = call->args.data[0]->as<AstExprConstantString>();
        return global != nullptr && name_view(global->name) == "require" ?
                   specifier :
                   nullptr;
    }

    static bool is_module_require(const AstExpr& expression) {
        return module_require_specifier(expression) != nullptr;
    }

    static bool is_native_module_require(const AstExpr& expression) {
        const auto* specifier = module_require_specifier(expression);
        return specifier != nullptr && is_native_luau_module(
                                           std::string_view {
                                               specifier->value.data,
                                               specifier->value.size,
                                           }
                                       );
    }

    bool is_direct_module_export(const AstExpr& expression) const {
        const auto* member = expression.as<AstExprIndexName>();
        const auto* module =
            member != nullptr ? member->expr->as<AstExprLocal>() : nullptr;
        return module != nullptr && m_module_imports.contains(module->local);
    }

    static bool accepts_imported_type_token(const AstExprCall& expression) {
        const auto* method = expression.func->as<AstExprIndexName>();
        if (expression.self && method != nullptr &&
            name_view(method->index) == "resource") {
            return true;
        }
        const auto* global = expression.func->as<AstExprGlobal>();
        if (global == nullptr) {
            return false;
        }
        static const std::unordered_set<std::string_view> functions {
            "Read",
            "Write",
            "With",
            "Without",
        };
        return functions.contains(name_view(global->name));
    }

    static bool append_path(
        const AstExpr& expression,
        std::vector<std::string_view>& path
    ) {
        if (const auto* global = expression.as<Luau::AstExprGlobal>()) {
            path.push_back(name_view(global->name));
            return true;
        }
        if (const auto* index = expression.as<AstExprIndexName>()) {
            if (!append_path(*index->expr, path)) {
                return false;
            }
            path.push_back(name_view(index->index));
            return true;
        }
        return false;
    }

    static std::string join_path(const std::vector<std::string_view>& path) {
        std::string result;
        for (const auto component : path) {
            if (!result.empty()) {
                result.push_back('.');
            }
            result.append(component);
        }
        return result;
    }

    bool validate_assignment(const AstExpr& target) {
        const auto* root = assignment_root(target);
        if (const auto* global = root->as<Luau::AstExprGlobal>()) {
            reject(
                target.location,
                "assignment to global '" +
                    std::string(name_view(global->name)) + "' is not allowed"
            );
            return false;
        }
        if (const auto* local = root->as<AstExprLocal>();
            local != nullptr &&
            m_native_module_imports.contains(local->local)) {
            reject(
                target.location,
                "assignment to readonly native module '" +
                    std::string(name_view(local->local->name)) +
                    "' is not allowed"
            );
            return false;
        }
        if (const auto* local = target.as<AstExprLocal>();
            local != nullptr &&
            m_readonly_module_bindings.contains(local->local)) {
            reject(
                target.location,
                "cannot reassign readonly module binding '" +
                    std::string(name_view(local->local->name)) + "'"
            );
            return false;
        }
        if (references_module_state(*root)) {
            const auto* local = root->as<AstExprLocal>();
            reject(
                target.location,
                "mutation of module state '" +
                    std::string(name_view(local->local->name)) +
                    "' is not allowed; store persistent state in an ECS "
                    "resource or component"
            );
            return false;
        }
        return true;
    }

    bool references_module_state(const AstExpr& expression) const {
        const auto* root = assignment_root(expression);
        const auto* local = root->as<AstExprLocal>();
        return local != nullptr &&
               ((local->upvalue && local->local->functionDepth == 0 &&
                 !m_native_module_imports.contains(local->local)) ||
                m_module_state_aliases.contains(local->local));
    }

    void update_alias(const AstExpr& target, const AstExpr& value) {
        const auto* local = target.as<AstExprLocal>();
        if (local == nullptr || local->upvalue) {
            return;
        }
        if (references_module_state(value)) {
            m_module_state_aliases.insert(local->local);
        } else {
            m_module_state_aliases.erase(local->local);
        }
    }

    void reject(const Luau::Location& location, std::string message) {
        if (error) {
            return;
        }
        error = LuauScriptError {
            source_name + ":" + std::to_string(location.begin.line + 1) +
                ": snapshot-unsafe Luau: " + std::move(message),
        };
    }
};

Status<LuauScriptError> validate_parsed_luau_snapshot_safety(
    const LuauScriptSource& source,
    Luau::AstStatBlock& root
) {
    LuauSnapshotSafetyVisitor visitor {root, source.name};
    root.visit(&visitor);
    if (visitor.error) {
        return failure(std::move(*visitor.error));
    }
    return {};
}

LuauScriptError declaration_error(std::string message) {
    return LuauScriptError {
        "Invalid Luau script declaration: " + std::move(message)
    };
}

std::string_view name_view(Luau::AstName name) {
    return name.value != nullptr ? std::string_view {name.value} :
                                   std::string_view {};
}

const AstTypeReference* type_reference(const AstType* type) {
    return type != nullptr ? type->as<AstTypeReference>() : nullptr;
}

const AstTypeReference*
required_type_argument(const AstTypeReference& wrapper, std::size_t index) {
    if (index >= wrapper.parameters.size ||
        wrapper.parameters.data[index].type == nullptr) {
        return nullptr;
    }
    return type_reference(wrapper.parameters.data[index].type);
}

DynamicTypeRef dynamic_type_ref(const AstTypeReference& type) {
    std::string name;
    if (type.prefix) {
        name.append(name_view(*type.prefix));
        name.append("::");
    }
    name.append(name_view(type.name));
    return DynamicTypeRef {.type_name = std::move(name)};
}

Result<void, LuauScriptError>
append_query_item(DynamicQueryParamDecl& query, const AstTypeReference& item) {
    const std::string_view kind = name_view(item.name);
    if (kind == "Entity") {
        query.fields.push_back(
            DynamicQueryFieldDecl {
                .name = "entity",
                .kind = DynamicQueryFieldDeclKind::Entity,
            }
        );
        return {};
    }

    if (kind == "Or") {
        if (item.parameters.size == 0) {
            return failure(declaration_error(
                "'Or' must have at least one filter type argument"
            ));
        }
        DynamicQueryFilterDecl filter {
            .kind = DynamicQueryFilterDecl::Kind::Or,
        };
        for (const Luau::AstTypeOrPack& argument : item.parameters) {
            const AstTypeReference* child = type_reference(argument.type);
            if (child == nullptr) {
                return failure(
                    declaration_error("Or arguments must be named filter types")
                );
            }
            DynamicQueryParamDecl nested;
            auto status = append_query_item(nested, *child);
            if (!status) {
                return status;
            }
            if (!nested.fields.empty() || nested.filters.size() != 1) {
                return failure(declaration_error(
                    "Or arguments must be With<T>, Without<T>, Added<T>, "
                    "Changed<T>, or Or<...>"
                ));
            }
            filter.filters.push_back(std::move(nested.filters.front()));
        }
        query.filters.push_back(std::move(filter));
        return {};
    }

    const AstTypeReference* value = required_type_argument(item, 0);
    if (value == nullptr || item.parameters.size != 1) {
        return failure(declaration_error(
            "'" + std::string(kind) + "' must have exactly one type argument"
        ));
    }

    if (kind == "Read" || kind == "Write") {
        query.fields.push_back(
            DynamicQueryFieldDecl {
                .name = std::string(name_view(value->name)),
                .type = dynamic_type_ref(*value),
                .access = kind == "Write" ? DynamicParamAccess::Write :
                                            DynamicParamAccess::Read,
            }
        );
        return {};
    }
    if (kind == "With" || kind == "Without" || kind == "Added" ||
        kind == "Changed") {
        auto filter_kind = DynamicQueryFilterDecl::Kind::With;
        if (kind == "Without") {
            filter_kind = DynamicQueryFilterDecl::Kind::Without;
        } else if (kind == "Added") {
            filter_kind = DynamicQueryFilterDecl::Kind::Added;
        } else if (kind == "Changed") {
            filter_kind = DynamicQueryFilterDecl::Kind::Changed;
        }
        query.filters.push_back(
            DynamicQueryFilterDecl {
                .kind = filter_kind,
                .type = dynamic_type_ref(*value),
                .required = kind == "With",
            }
        );
        return {};
    }

    return failure(declaration_error(
        "unsupported Query item type '" + std::string(kind) + "'"
    ));
}

Result<std::unique_ptr<DynamicQueryParamDecl>, LuauScriptError>
compile_query(const AstTypeReference& annotation, std::string param_name) {
    auto result = std::make_unique<DynamicQueryParamDecl>();
    result->name = std::move(param_name);

    const AstTypeReference* query = &annotation;
    if (name_view(annotation.name) == "Filtered") {
        query = required_type_argument(annotation, 0);
        if (query == nullptr || name_view(query->name) != "Query") {
            return failure(declaration_error(
                "Filtered's first type argument must be Query<...>"
            ));
        }
    }

    if (name_view(query->name) != "Query") {
        return failure(declaration_error("expected Query<...>"));
    }
    for (const Luau::AstTypeOrPack& argument : query->parameters) {
        const AstTypeReference* item = type_reference(argument.type);
        if (item == nullptr) {
            return failure(
                declaration_error("Query arguments must be named types")
            );
        }
        auto status = append_query_item(*result, *item);
        if (!status) {
            return failure(std::move(status.error()));
        }
    }

    if (name_view(annotation.name) == "Filtered") {
        for (std::size_t index = 1; index < annotation.parameters.size;
             ++index) {
            const AstTypeReference* filter =
                required_type_argument(annotation, index);
            if (filter == nullptr) {
                return failure(declaration_error(
                    "Filtered arguments must be query filter types"
                ));
            }
            auto status = append_query_item(*result, *filter);
            if (!status) {
                return failure(std::move(status.error()));
            }
        }
    }
    return result;
}

struct UnwrappedAnnotation {
    const AstTypeReference* type {nullptr};
    bool optional {false};
};

UnwrappedAnnotation unwrap_annotation(const AstType* annotation) {
    if (const auto* reference = type_reference(annotation)) {
        return {.type = reference};
    }
    if (const auto* union_type =
            annotation != nullptr ? annotation->as<AstTypeUnion>() : nullptr) {
        const AstTypeReference* value = nullptr;
        bool has_nil = false;
        for (const AstType* member : union_type->types) {
            const AstTypeReference* reference = type_reference(member);
            if (member->as<AstTypeOptional>() != nullptr ||
                (reference != nullptr && name_view(reference->name) == "nil")) {
                has_nil = true;
            } else if (reference != nullptr && value == nullptr) {
                value = reference;
            } else {
                return {};
            }
        }
        return {.type = value, .optional = has_nil && value != nullptr};
    }
    return {};
}

Result<DynamicSystemParamDeclPtr, LuauScriptError>
compile_param(const AstLocal& param) {
    const UnwrappedAnnotation annotation = unwrap_annotation(param.annotation);
    if (annotation.type == nullptr) {
        return failure(declaration_error(
            "system parameter '" + std::string(name_view(param.name)) +
            "' requires a supported type annotation"
        ));
    }

    const std::string_view kind = name_view(annotation.type->name);
    const std::string param_name {name_view(param.name)};
    if (kind == "Query" || kind == "Filtered") {
        auto query = compile_query(*annotation.type, param_name);
        if (!query) {
            return failure(std::move(query.error()));
        }
        return DynamicSystemParamDeclPtr {std::move(*query)};
    }
    if (kind == "ResRO" || kind == "ResRW") {
        const AstTypeReference* value =
            required_type_argument(*annotation.type, 0);
        if (value == nullptr || annotation.type->parameters.size != 1) {
            return failure(declaration_error(
                "'" + std::string(kind) +
                "' must have exactly one type argument"
            ));
        }
        auto resource = std::make_unique<DynamicResourceParamDecl>();
        resource->name = param_name;
        resource->type = dynamic_type_ref(*value);
        resource->access = kind == "ResRW" ? DynamicParamAccess::Write :
                                             DynamicParamAccess::Read;
        resource->optional = annotation.optional;
        return DynamicSystemParamDeclPtr {std::move(resource)};
    }
    if (kind == "State" || kind == "NextState") {
        const AstTypeReference* value =
            required_type_argument(*annotation.type, 0);
        if (value == nullptr || annotation.type->parameters.size != 1) {
            return failure(declaration_error(
                "'" + std::string(kind) +
                "' must have exactly one type argument"
            ));
        }
        if (kind == "State") {
            auto state = std::make_unique<DynamicStateParamDecl>();
            state->name = param_name;
            state->type = dynamic_type_ref(*value);
            return DynamicSystemParamDeclPtr {std::move(state)};
        }
        auto next_state = std::make_unique<DynamicNextStateParamDecl>();
        next_state->name = param_name;
        next_state->type = dynamic_type_ref(*value);
        return DynamicSystemParamDeclPtr {std::move(next_state)};
    }
    if (kind == "RemovedComponents") {
        const AstTypeReference* value =
            required_type_argument(*annotation.type, 0);
        if (value == nullptr || annotation.type->parameters.size != 1) {
            return failure(declaration_error(
                "'RemovedComponents' must have exactly one type argument"
            ));
        }
        auto removed = std::make_unique<DynamicRemovedComponentsParamDecl>();
        removed->name = param_name;
        removed->type = dynamic_type_ref(*value);
        return DynamicSystemParamDeclPtr {std::move(removed)};
    }
    if (kind == "EventWriter" || kind == "EventReader" ||
        kind == "EventReaderRO") {
        const AstTypeReference* value =
            required_type_argument(*annotation.type, 0);
        if (value == nullptr || annotation.type->parameters.size != 1) {
            return failure(declaration_error(
                "'" + std::string(kind) +
                "' must have exactly one type argument"
            ));
        }
        if (annotation.optional && kind != "EventReaderRO") {
            return failure(
                declaration_error("only EventReaderRO<T> may be optional")
            );
        }
        auto event = std::make_unique<DynamicEventParamDecl>();
        event->name = param_name;
        event->type = dynamic_type_ref(*value);
        event->optional = annotation.optional;
        if (kind == "EventWriter") {
            event->kind = DynamicEventParamDeclKind::Writer;
        } else if (kind == "EventReader") {
            event->kind = DynamicEventParamDeclKind::Reader;
        } else {
            event->kind = DynamicEventParamDeclKind::ReaderRO;
        }
        return DynamicSystemParamDeclPtr {std::move(event)};
    }
    if (kind == "Commands" && annotation.type->parameters.size == 0) {
        auto commands = std::make_unique<DynamicCommandsParamDecl>();
        commands->name = param_name;
        return DynamicSystemParamDeclPtr {std::move(commands)};
    }
    if (kind == "World" && annotation.type->parameters.size == 0) {
        auto world = std::make_unique<DynamicWorldParamDecl>();
        world->name = param_name;
        return DynamicSystemParamDeclPtr {std::move(world)};
    }
    return failure(declaration_error(
        "unsupported system parameter type '" + std::string(kind) + "'"
    ));
}

Result<ScheduleId, LuauScriptError> schedule_id(
    const AstExpr& expression,
    detail::luau_compiler::StateLoweringContext& state_lowering
) {
    std::string_view name;
    if (const auto* global = expression.as<AstExprGlobal>()) {
        name = name_view(global->name);
    } else if (const auto* index = expression.as<AstExprIndexName>()) {
        name = name_view(index->index);
    }

    static const std::unordered_map<std::string_view, ScheduleId> schedules {
        {"First", First},
        {"PreStartUp", PreStartUp},
        {"StartUp", StartUp},
        {"PreUpdate", PreUpdate},
        {"Update", Update},
        {"PostUpdate", PostUpdate},
        {"Last", Last},
        {"RenderPrepare", RenderPrepare},
        {"RenderFirst", RenderFirst},
        {"RenderStart", RenderStart},
        {"RenderUpdate", RenderUpdate},
        {"RenderEnd", RenderEnd},
        {"RenderLast", RenderLast},
        {"RunFixedMainLoop", RunFixedMainLoop},
        {"FixedFirst", FixedFirst},
        {"FixedPreUpdate", FixedPreUpdate},
        {"FixedUpdate", FixedUpdate},
        {"FixedPostUpdate", FixedPostUpdate},
        {"FixedLast", FixedLast},
    };
    const auto found = schedules.find(name);
    if (found != schedules.end()) {
        return found->second;
    }

    const auto* call = expression.as<AstExprCall>();
    const auto* callee =
        call != nullptr ? call->func->as<AstExprGlobal>() : nullptr;
    const std::string_view schedule_name =
        callee != nullptr ? name_view(callee->name) : std::string_view {};
    if (schedule_name == "OnEnter" || schedule_name == "OnExit") {
        if (call->args.size != 1) {
            return failure(declaration_error(
                std::string(schedule_name) + " requires exactly one state value"
            ));
        }
        auto value = state_lowering.compile_value(*call->args.data[0]);
        if (!value) {
            return failure(std::move(value.error()));
        }
        auto state = resolve_dynamic_state(value->type_id());
        return schedule_name == "OnEnter" ? state->on_enter(value->ref()) :
                                            state->on_exit(value->ref());
    }
    if (schedule_name == "OnTransition") {
        if (call->args.size != 2) {
            return failure(declaration_error(
                "OnTransition requires exited and entered state values"
            ));
        }
        auto exited = state_lowering.compile_value(*call->args.data[0]);
        if (!exited) {
            return failure(std::move(exited.error()));
        }
        auto entered = state_lowering.compile_value(*call->args.data[1]);
        if (!entered) {
            return failure(std::move(entered.error()));
        }
        if (exited->type_id() != entered->type_id()) {
            return failure(declaration_error(
                "OnTransition state values must have the same type"
            ));
        }
        auto state = resolve_dynamic_state(exited->type_id());
        return state->on_transition(exited->ref(), entered->ref());
    }
    return failure(declaration_error("unknown schedule declaration"));
}

Result<std::string, LuauScriptError>
record_name(const AstExprTable::Item& item, std::string_view context) {
    if (item.kind != AstExprTable::Item::Kind::Record || item.key == nullptr) {
        return failure(declaration_error(
            std::string {context} + " entries must use named fields"
        ));
    }
    const auto* key = item.key->as<AstExprConstantString>();
    if (key == nullptr || key->value.size == 0) {
        return failure(declaration_error(
            std::string {context} + " names must be non-empty strings"
        ));
    }
    return std::string {key->value.data, key->value.size};
}

std::string normalize_primitive_name(std::string name) {
    if (name == "str") {
        return "string";
    }
    return name;
}

Result<Val, LuauScriptError> compile_resource_value(const AstExpr& expression) {
    if (const auto* boolean = expression.as<AstExprConstantBool>()) {
        return make_val<bool>(boolean->value);
    }
    if (const auto* number = expression.as<AstExprConstantNumber>()) {
        const double value = number->value;
        if (std::trunc(value) == value &&
            value >= static_cast<double>(std::numeric_limits<int>::min()) &&
            value <= static_cast<double>(std::numeric_limits<int>::max())) {
            return make_val<int>(static_cast<int>(value));
        }
        return make_val<float>(static_cast<float>(value));
    }
    if (const auto* text = expression.as<AstExprConstantString>()) {
        return make_val<std::string>(text->value.data, text->value.size);
    }
    return failure(declaration_error(
        "resource initial values must be boolean, number, or string literals"
    ));
}

using detail::luau_compiler::ModuleImportBindings;

ModuleImportBindings collect_import_locals(const Luau::AstStatBlock& root);

struct FunctionResolutionContext {
    const ModuleIR& module;
    const LuauModuleMetadataResolver& module_metadata_resolver;
};

Result<std::vector<DynamicSystemParamDeclPtr>, LuauScriptError>
compile_function_params(const AstExprFunction& function) {
    std::vector<DynamicSystemParamDeclPtr> params;
    params.reserve(function.args.size);
    for (const AstLocal* param : function.args) {
        auto compiled = compile_param(*param);
        if (!compiled) {
            return failure(std::move(compiled.error()));
        }
        params.push_back(std::move(*compiled));
    }
    return params;
}

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

Result<LuauFunctionDecl, LuauScriptError> compile_function_ref(
    const AstExpr& expression,
    const FunctionResolutionContext& function_context,
    std::string_view diagnostic_context
) {
    const auto* function_ref = expression.as<AstExprLocal>();
    const auto* function =
        function_ref != nullptr ?
            function_context.module.find_function(function_ref->local) :
            nullptr;
    if (function != nullptr) {
        auto params = compile_function_params(*function->expression);
        if (!params) {
            return failure(std::move(params.error()));
        }
        return LuauFunctionDecl {
            .name = std::string(name_view(function_ref->local->name)),
            .params = std::move(*params),
        };
    }

    const auto* member = expression.as<AstExprIndexName>();
    const auto* module =
        member != nullptr ? member->expr->as<AstExprLocal>() : nullptr;
    const auto& imports = function_context.module.imports();
    const auto imported =
        module != nullptr ? imports.find(module->local) : imports.end();
    if (member == nullptr || imported == imports.end()) {
        return failure(declaration_error(
            std::string(diagnostic_context) +
            " must name a top-level local or imported exported function"
        ));
    }
    if (!function_context.module_metadata_resolver) {
        return failure(declaration_error(
            std::string(diagnostic_context) +
            " references imported function '" + imported->second + "." +
            std::string(name_view(member->index)) +
            "' without a module resolver"
        ));
    }
    auto metadata = function_context.module_metadata_resolver(imported->second);
    if (!metadata) {
        return failure(std::move(metadata.error()));
    }
    const std::string_view export_name = name_view(member->index);
    const auto* exported = (*metadata)->find_function(export_name);
    if (exported == nullptr) {
        return failure(declaration_error(
            "module '" + (*metadata)->schema.source_name +
            "' does not export function '" + std::string(export_name) + "'"
        ));
    }
    if (!exported->is_system_compatible()) {
        return failure(declaration_error(
            "exported function '" + exported->qualified_name +
            "' is not system-compatible: " + exported->system_signature_error
        ));
    }
    auto params = clone_param_declarations(exported->system_params);
    return LuauFunctionDecl {
        .name = exported->qualified_name,
        .params = std::move(params),
    };
}

bool is_read_only_condition_param(const DynamicSystemParamDecl& param) {
    if (param.decl_type_id() == type_id<DynamicResourceParamDecl>()) {
        return static_cast<const DynamicResourceParamDecl&>(param).access ==
               DynamicParamAccess::Read;
    }
    if (param.decl_type_id() == type_id<DynamicQueryParamDecl>()) {
        const auto& query = static_cast<const DynamicQueryParamDecl&>(param);
        return std::ranges::all_of(query.fields, [](const auto& field) {
            return field.access == DynamicParamAccess::Read;
        });
    }
    if (param.decl_type_id() == type_id<DynamicStateParamDecl>()) {
        return true;
    }
    return false;
}

Result<DynamicConditionDecl, LuauScriptError> compile_condition(
    const AstExpr& expression,
    const FunctionResolutionContext& function_context,
    detail::luau_compiler::StateLoweringContext& state_lowering
) {
    const auto* call = expression.as<AstExprCall>();
    const auto* callee =
        call != nullptr ? call->func->as<AstExprGlobal>() : nullptr;
    if (callee != nullptr && name_view(callee->name) == "in_state") {
        if (call->args.size != 1) {
            return failure(
                declaration_error("in_state requires exactly one state value")
            );
        }
        auto value = state_lowering.compile_value(*call->args.data[0]);
        if (!value) {
            return failure(std::move(value.error()));
        }
        auto param = std::make_unique<DynamicStateParamDecl>();
        param->name = "state";
        param->type.type_id = value->type_id();
        DynamicConditionDecl result {
            .kind = DynamicConditionDeclKind::InState,
            .name = "in_state",
            .state_value = std::move(*value),
        };
        result.params.push_back(std::move(param));
        return result;
    }

    auto function =
        compile_function_ref(expression, function_context, "run_if argument");
    if (!function) {
        return failure(std::move(function.error()));
    }
    if (!std::ranges::all_of(function->params, [](const auto& param) {
            return param && is_read_only_condition_param(*param);
        })) {
        return failure(declaration_error(
            "condition '" + function->name +
            "' may only use read-only resource and query parameters"
        ));
    }
    return DynamicConditionDecl {
        .name = std::move(function->name),
        .params = std::move(function->params),
    };
}

Result<DynamicSystemDecl, LuauScriptError> compile_bare_system(
    const AstExpr& expression,
    ScheduleId schedule,
    const FunctionResolutionContext& function_context,
    detail::luau_compiler::StateLoweringContext& state_lowering
) {
    struct Modifier {
        std::string_view name;
        const AstExprCall* call {nullptr};
    };

    const AstExpr* base = &expression;
    std::vector<Modifier> modifiers;
    while (const auto* call = base->as<AstExprCall>()) {
        const auto* method = call->func->as<AstExprIndexName>();
        if (method == nullptr || !call->self) {
            break;
        }
        const std::string_view method_name = name_view(method->index);
        if (method_name != "before" && method_name != "after" &&
            method_name != "run_if") {
            return failure(declaration_error(
                "unsupported system configuration method '" +
                std::string(method_name) + "'"
            ));
        }
        if (call->args.size == 0) {
            return failure(declaration_error(
                "system configuration method '" + std::string(method_name) +
                "' requires at least one function"
            ));
        }
        modifiers.push_back(Modifier {.name = method_name, .call = call});
        base = method->expr;
    }
    std::ranges::reverse(modifiers);

    auto function =
        compile_function_ref(*base, function_context, "system entry");
    if (!function) {
        return failure(std::move(function.error()));
    }
    DynamicSystemDecl result {
        .name = std::move(function->name),
        .params = std::move(function->params),
        .schedule = schedule,
    };

    for (const auto& modifier : modifiers) {
        for (const AstExpr* argument : modifier.call->args) {
            if (modifier.name == "run_if") {
                auto condition = compile_condition(
                    *argument,
                    function_context,
                    state_lowering
                );
                if (!condition) {
                    return failure(std::move(condition.error()));
                }
                result.conditions.push_back(std::move(*condition));
                continue;
            }
            auto target = compile_function_ref(
                *argument,
                function_context,
                std::string(modifier.name) + " argument"
            );
            if (!target) {
                return failure(std::move(target.error()));
            }
            auto& dependencies =
                modifier.name == "before" ? result.before : result.after;
            dependencies.push_back(std::move(target->name));
        }
    }
    return result;
}

struct CompiledSystemGroup {
    std::vector<DynamicSystemDecl> systems;
    std::vector<std::string> first;
    std::vector<std::string> last;
};

Result<CompiledSystemGroup, LuauScriptError> compile_system_group(
    const AstExpr& expression,
    ScheduleId schedule,
    const FunctionResolutionContext& function_context,
    detail::luau_compiler::StateLoweringContext& state_lowering
) {
    const auto* call = expression.as<AstExprCall>();
    const auto* callee =
        call != nullptr ? call->func->as<AstExprGlobal>() : nullptr;
    if (call == nullptr || callee == nullptr ||
        name_view(callee->name) != "chain") {
        auto system = compile_bare_system(
            expression,
            schedule,
            function_context,
            state_lowering
        );
        if (!system) {
            return failure(std::move(system.error()));
        }
        const std::string name = system->name;
        CompiledSystemGroup result;
        result.systems.push_back(std::move(*system));
        result.first.push_back(name);
        result.last.push_back(name);
        return result;
    }
    if (call->args.size < 2) {
        return failure(
            declaration_error("chain requires at least two system groups")
        );
    }

    std::vector<CompiledSystemGroup> groups;
    groups.reserve(call->args.size);
    for (const AstExpr* argument : call->args) {
        auto group = compile_system_group(
            *argument,
            schedule,
            function_context,
            state_lowering
        );
        if (!group) {
            return failure(std::move(group.error()));
        }
        groups.push_back(std::move(*group));
    }
    for (std::size_t index = 0; index + 1 < groups.size(); ++index) {
        auto& former = groups[index];
        const auto& latter = groups[index + 1];
        for (const auto& source_name : former.last) {
            auto source = std::ranges::find(
                former.systems,
                source_name,
                &DynamicSystemDecl::name
            );
            if (source == former.systems.end()) {
                return failure(declaration_error(
                    "chain failed to resolve system '" + source_name + "'"
                ));
            }
            source->before.insert(
                source->before.end(),
                latter.first.begin(),
                latter.first.end()
            );
        }
    }

    CompiledSystemGroup result {
        .first = groups.front().first,
        .last = groups.back().last,
    };
    for (auto& group : groups) {
        result.systems.insert(
            result.systems.end(),
            std::make_move_iterator(group.systems.begin()),
            std::make_move_iterator(group.systems.end())
        );
    }
    return result;
}

void append_system_runtime_entries(
    const AstExpr& expression,
    std::vector<const AstExpr*>& entries
) {
    const auto* call = expression.as<AstExprCall>();
    const auto* callee =
        call != nullptr ? call->func->as<AstExprGlobal>() : nullptr;
    if (call == nullptr || callee == nullptr ||
        name_view(callee->name) != "chain") {
        entries.push_back(&expression);
        return;
    }
    for (const AstExpr* argument : call->args) {
        append_system_runtime_entries(*argument, entries);
    }
}

Result<LuauTypeRef, LuauScriptError> compile_exported_field_type(
    const AstType& annotation,
    const std::string& module_name,
    const std::unordered_set<std::string>& local_types
) {
    const AstType* value = &annotation;
    bool optional = false;
    if (const auto* union_type = annotation.as<AstTypeUnion>()) {
        const AstTypeReference* reference = nullptr;
        bool has_nil = false;
        for (const AstType* member : union_type->types) {
            if (member->is<AstTypeOptional>()) {
                has_nil = true;
            } else if (
                const auto* candidate = member->as<AstTypeReference>();
                candidate != nullptr && reference == nullptr
            ) {
                reference = candidate;
            } else {
                return failure(declaration_error(
                    "Entisium runtime fields must use a named type or T?"
                ));
            }
        }
        if (!has_nil || reference == nullptr) {
            return failure(declaration_error(
                "Entisium runtime fields must use a named type or T?"
            ));
        }
        value = reference;
        optional = true;
    }

    const auto* reference = value->as<AstTypeReference>();
    if (reference == nullptr ||
        (reference->hasParameterList &&
         !detail::luau_schema::is_runtime_asset_handle(*reference))) {
        return failure(declaration_error(
            "Entisium runtime fields must use non-generic named types"
        ));
    }
    if (reference->hasParameterList) {
        return LuauTypeRef {
            .type_name = "UntypedHandle",
            .type_id = type_id<UntypedHandle>(),
        };
    }
    std::string type_name;
    if (reference->prefix) {
        type_name.append(name_view(*reference->prefix));
        type_name.append("::");
    }
    type_name.append(name_view(reference->name));
    type_name = normalize_primitive_name(std::move(type_name));
    const bool local_type =
        !reference->prefix && local_types.contains(type_name);
    if (local_type) {
        type_name = module_name + "." + type_name;
    }
    const bool entity_type = type_name == "entity";
    if (optional && !entity_type) {
        return failure(declaration_error(
            "optional Entisium runtime fields currently support only entity "
            "values"
        ));
    }
    return LuauTypeRef {
        .type_name = std::move(type_name),
        .type_id = entity_type ? Optional<TypeId> {type_id<Entity>()} : nullopt,
        .script_type = local_type,
        .optional = optional,
    };
}

Result<std::vector<TypeDeclIR>, LuauScriptError> compile_exported_types(
    const Luau::AstStatBlock& root,
    const std::string& module_name
) {
    std::unordered_map<std::string, std::size_t> name_counts;
    std::unordered_set<std::string> names;
    for (const AstStat* statement : root.body) {
        const auto* alias = statement->as<AstStatTypeAlias>();
        if (alias == nullptr || !alias->exported) {
            continue;
        }
        const std::string name {name_view(alias->name)};
        ++name_counts[name];
        names.insert(name);
    }

    std::vector<TypeDeclIR> result;
    std::unordered_set<std::string> emitted;
    for (const AstStat* statement : root.body) {
        const auto* alias = statement->as<AstStatTypeAlias>();
        if (alias == nullptr || !alias->exported) {
            continue;
        }
        const std::string name {name_view(alias->name)};
        if (!emitted.insert(name).second) {
            continue;
        }
        std::string qualified_name {module_name};
        qualified_name.push_back('.');
        qualified_name.append(name);
        if (name_counts[name] != 1) {
            std::string runtime_error {"duplicate type alias '"};
            runtime_error.append(name);
            runtime_error.append(
                "' cannot be used as an Entisium runtime type"
            );
            result.push_back(
                TypeDeclIR {
                    .name = name,
                    .qualified_name = std::move(qualified_name),
                    .value = UnsupportedTypeIR {
                        .runtime_error = std::move(runtime_error),
                    },
                }
            );
            continue;
        }

        auto analysis = detail::luau_schema::analyze_exported_type(*alias);
        if (!analysis.runtime_compatible()) {
            result.push_back(
                TypeDeclIR {
                    .name = name,
                    .qualified_name = std::move(qualified_name),
                    .value = UnsupportedTypeIR {
                        .runtime_error = std::move(analysis.runtime_error),
                    },
                }
            );
            continue;
        }
        auto& shape = analysis.shape;
        if (shape.kind == detail::luau_schema::ExportedTypeKind::StringUnion) {
            result.push_back(
                TypeDeclIR {
                    .name = name,
                    .qualified_name = std::move(qualified_name),
                    .value = StringUnionTypeIR {
                        .values = std::move(shape.string_values),
                    },
                }
            );
            continue;
        }
        const auto* table = alias->type->as<AstTypeTable>();
        TypeDeclIR type {
            .name = name,
            .qualified_name = std::move(qualified_name),
            .value = RecordTypeIR {},
        };
        auto& record = std::get<RecordTypeIR>(type.value);
        std::string runtime_error;
        for (const auto& property : table->props) {
            const std::string field_name {name_view(property.name)};
            auto field_type =
                compile_exported_field_type(*property.type, module_name, names);
            if (!field_type) {
                runtime_error = std::move(field_type.error().message);
                break;
            }
            record.fields.push_back(
                LuauFieldDecl {
                    .name = field_name,
                    .type = std::move(*field_type),
                }
            );
        }
        if (runtime_error.empty()) {
            std::ranges::sort(record.fields, {}, &LuauFieldDecl::name);
        } else {
            type.value = UnsupportedTypeIR {
                .runtime_error = std::move(runtime_error),
            };
        }
        result.push_back(std::move(type));
    }
    std::ranges::sort(result, {}, &TypeDeclIR::name);
    return result;
}

[[nodiscard]] bool is_runtime_value_method(std::string_view name) {
    static constexpr std::string_view names[] {
        "add_resource",
        "insert_resource",
        "init_state",
        "insert_state",
        "set_resource",
    };
    return std::ranges::find(names, name) != std::ranges::end(names);
}

[[nodiscard]] bool is_runtime_type_method(std::string_view name) {
    static constexpr std::string_view names[] {
        "add_event",
        "has_resource",
        "resource",
    };
    return std::ranges::find(names, name) != std::ranges::end(names);
}

[[nodiscard]] bool is_runtime_type_function(std::string_view name) {
    static constexpr std::string_view names[] {
        "Added",
        "Changed",
        "Read",
        "With",
        "Without",
        "Write",
        "field",
        "optional",
    };
    return std::ranges::find(names, name) != std::ranges::end(names);
}

[[nodiscard]] const AstExpr*
runtime_value_type_reference(const AstExpr& expression) {
    if (const auto* call = expression.as<AstExprCall>()) {
        const AstExpr* callee = call->func;
        if (const auto* member = callee->as<AstExprIndexName>();
            member != nullptr && name_view(member->index) == "new") {
            callee = member->expr;
        }
        return callee;
    }
    if (const auto* member = expression.as<AstExprIndexName>()) {
        return member->expr;
    }
    return nullptr;
}

template<typename Visitor>
bool visit_runtime_type_references(
    const AstExprCall& expression,
    Visitor&& visitor
) {
    if (expression.self) {
        const auto* method = expression.func->as<AstExprIndexName>();
        if (method == nullptr) {
            return true;
        }
        const std::string_view name = name_view(method->index);
        if (is_runtime_value_method(name)) {
            for (const AstExpr* argument : expression.args) {
                const AstExpr* reference =
                    runtime_value_type_reference(*argument);
                if (reference != nullptr && !visitor(*reference)) {
                    return false;
                }
            }
        } else if (
            is_runtime_type_method(name) && expression.args.size != 0 &&
            !visitor(*expression.args.data[0])
        ) {
            return false;
        }
        return true;
    }

    const auto* function = expression.func->as<AstExprGlobal>();
    if (function != nullptr &&
        is_runtime_type_function(name_view(function->name)) &&
        expression.args.size != 0) {
        return visitor(*expression.args.data[0]);
    }
    return true;
}

class LocalRuntimeTypeUseValidator final : public Luau::AstVisitor {
  public:
    explicit LocalRuntimeTypeUseValidator(
        const std::vector<TypeDeclIR>& types
    ) {
        for (const auto& type : types) {
            if (const auto* unsupported =
                    std::get_if<UnsupportedTypeIR>(&type.value)) {
                m_unsupported.emplace(type.name, unsupported->runtime_error);
            }
        }
    }

    bool visit(AstExprCall* expression) override {
        if (m_error) {
            return false;
        }
        return visit_runtime_type_references(
            *expression,
            [&](const AstExpr& reference) {
                const auto* global = reference.as<AstExprGlobal>();
                if (global == nullptr) {
                    return true;
                }
                const auto found =
                    m_unsupported.find(std::string {name_view(global->name)});
                if (found == m_unsupported.end()) {
                    return true;
                }
                m_error = declaration_error(
                    "exported type '" + found->first +
                    "' cannot be used by an Entisium runtime API: " +
                    found->second
                );
                return false;
            }
        );
    }

    [[nodiscard]] const Optional<LuauScriptError>& error() const {
        return m_error;
    }

  private:
    std::unordered_map<std::string, std::string> m_unsupported;
    Optional<LuauScriptError> m_error;
};

Status<LuauScriptError> validate_local_runtime_type_uses(
    Luau::AstStatBlock& root,
    const std::vector<TypeDeclIR>& types
) {
    LocalRuntimeTypeUseValidator validator {types};
    root.visit(&validator);
    if (validator.error()) {
        return failure(*validator.error());
    }
    return {};
}

class ImportedRuntimeTypeUseValidator final : public Luau::AstVisitor {
  public:
    ImportedRuntimeTypeUseValidator(
        const ModuleIR& module,
        const LuauModuleMetadataResolver& resolver
    ) : m_module(&module), m_resolver(&resolver) {}

    bool visit(AstExprCall* expression) override {
        if (m_error || !*m_resolver) {
            return !m_error;
        }
        return visit_runtime_type_references(
            *expression,
            [&](const AstExpr& reference) {
                return validate(reference);
            }
        );
    }

    [[nodiscard]] Optional<LuauScriptError>& error() { return m_error; }

  private:
    bool validate(const AstExpr& reference) {
        const auto* member = reference.as<AstExprIndexName>();
        const auto* module =
            member != nullptr ? member->expr->as<AstExprLocal>() : nullptr;
        const auto imported = module != nullptr ?
                                  m_module->imports().find(module->local) :
                                  m_module->imports().end();
        if (imported == m_module->imports().end()) {
            return true;
        }
        if (is_native_luau_module(imported->second)) {
            return true;
        }

        const auto cached = m_metadata.find(imported->second);
        std::shared_ptr<const LuauModuleMetadata> metadata;
        if (cached != m_metadata.end()) {
            metadata = cached->second;
        } else {
            auto resolved = (*m_resolver)(imported->second);
            if (!resolved) {
                m_error = std::move(resolved.error());
                return false;
            }
            metadata = std::move(*resolved);
            m_metadata.emplace(imported->second, metadata);
        }

        const std::string_view name = name_view(member->index);
        const auto* type = metadata->find_exported_type(name);
        if (type == nullptr || type->runtime_compatible) {
            return true;
        }
        m_error = declaration_error(
            "exported type '" + type->name + "' from module '" +
            metadata->schema.source_name +
            "' cannot be used by an Entisium runtime API: " +
            type->runtime_error
        );
        return false;
    }
    const ModuleIR* m_module;
    const LuauModuleMetadataResolver* m_resolver;
    std::unordered_map<std::string, std::shared_ptr<const LuauModuleMetadata>>
        m_metadata;
    Optional<LuauScriptError> m_error;
};

Status<LuauScriptError> validate_imported_runtime_type_uses(
    Luau::AstStatBlock& root,
    const ModuleIR& module,
    const LuauModuleMetadataResolver& resolver
) {
    if (!resolver) {
        return {};
    }
    ImportedRuntimeTypeUseValidator validator {module, resolver};
    root.visit(&validator);
    if (validator.error()) {
        return failure(std::move(*validator.error()));
    }
    return {};
}

ModuleImportBindings collect_import_locals(const Luau::AstStatBlock& root) {
    ModuleImportBindings result;
    for (const AstStat* statement : root.body) {
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
                local->vars.data[index],
                std::string {specifier->value.data, specifier->value.size}
            );
        }
    }
    return result;
}

std::vector<FunctionIR>
collect_top_level_functions(const Luau::AstStatBlock& root) {
    std::vector<FunctionIR> result;
    for (const AstStat* statement : root.body) {
        const auto* function = statement->as<AstStatLocalFunction>();
        if (function == nullptr) {
            continue;
        }
        result.push_back(
            FunctionIR {
                .name = std::string(name_view(function->name->name)),
                .local = function->name,
                .expression = function->func,
                .exported = function->name->isExported,
            }
        );
    }
    return result;
}

Result<std::vector<PluginDependencyIR>, LuauScriptError>
compile_plugin_dependencies(
    const PluginIR& plugin,
    const ModuleImportBindings& imports,
    const std::unordered_map<const AstLocal*, std::string>& local_plugins
) {
    const auto dependencies_value =
        plugin.descriptor->getRecord("dependencies");
    if (!dependencies_value) {
        return std::vector<PluginDependencyIR> {};
    }
    const auto* dependencies = (*dependencies_value)->as<AstExprTable>();
    if (dependencies == nullptr) {
        return failure(declaration_error(
            "Plugin dependencies must be an array of imported plugins"
        ));
    }
    std::vector<PluginDependencyIR> result;
    for (const auto& item : dependencies->items) {
        if (item.kind != AstExprTable::Item::Kind::List) {
            return failure(
                declaration_error("Plugin dependencies must be an array")
            );
        }
        if (const auto* local = item.value->as<AstExprLocal>()) {
            const auto dependency = local_plugins.find(local->local);
            if (dependency == local_plugins.end()) {
                return failure(declaration_error(
                    "local Plugin dependency must name an exported Plugin"
                ));
            }
            result.push_back(
                PluginDependencyIR {
                    .plugin_name = dependency->second,
                }
            );
            continue;
        }
        const auto* member = item.value->as<AstExprIndexName>();
        const auto* module =
            member != nullptr ? member->expr->as<AstExprLocal>() : nullptr;
        const auto imported =
            module != nullptr ? imports.find(module->local) : imports.end();
        if (member == nullptr || imported == imports.end()) {
            return failure(declaration_error(
                "Plugin dependency must use Module.ExportedPlugin from require"
            ));
        }
        result.push_back(
            PluginDependencyIR {
                .import_specifier = imported->second,
                .plugin_name = std::string(name_view(member->index)),
            }
        );
    }
    return result;
}

Result<std::vector<PluginIR>, LuauScriptError>
find_exported_plugins(const Luau::AstStatBlock& root) {
    std::vector<PluginIR> result;
    std::unordered_set<std::string> names;
    for (const AstStat* statement : root.body) {
        const auto* local = statement->as<AstStatLocal>();
        if (local == nullptr) {
            continue;
        }
        const auto count = std::min(local->vars.size, local->values.size);
        for (std::size_t index = 0; index < count; ++index) {
            const AstLocal* variable = local->vars.data[index];
            if (!variable->isExported) {
                continue;
            }
            const auto* call = local->values.data[index]->as<AstExprCall>();
            const auto* member =
                call != nullptr ? call->func->as<AstExprIndexName>() : nullptr;
            const auto* owner =
                member != nullptr ? member->expr->as<AstExprGlobal>() : nullptr;
            if (call == nullptr || member == nullptr || owner == nullptr ||
                name_view(owner->name) != "Plugin" ||
                name_view(member->index) != "new") {
                continue;
            }
            const std::string plugin_name {name_view(variable->name)};
            if (!names.insert(plugin_name).second) {
                return failure(declaration_error(
                    "duplicate exported Plugin '" + plugin_name + "'"
                ));
            }
            if (call->args.size != 1) {
                return failure(declaration_error(
                    "Plugin.new expects exactly one descriptor table"
                ));
            }
            const auto* descriptor = call->args.data[0]->as<AstExprTable>();
            if (descriptor == nullptr) {
                return failure(
                    declaration_error("Plugin.new expects a descriptor table")
                );
            }
            const auto build_value = descriptor->getRecord("build");
            const auto* build =
                build_value ? (*build_value)->as<AstExprFunction>() : nullptr;
            if (build == nullptr || build->args.size != 1) {
                return failure(declaration_error(
                    "Plugin.new requires build = function(app) ... end"
                ));
            }
            result.push_back(
                PluginIR {
                    .name = plugin_name,
                    .local = variable,
                    .descriptor = descriptor,
                    .build = build,
                }
            );
        }
    }
    return result;
}

struct RuntimeFunctionCatalog {
    LuauPluginDecl plugin;
    std::vector<std::string> expressions;
};

std::string expression_source(
    const LuauScriptSource& source,
    const Luau::Location& location
) {
    std::vector<std::size_t> line_starts {0};
    for (std::size_t index = 0; index < source.content.size(); ++index) {
        if (source.content[index] == '\n') {
            line_starts.push_back(index + 1);
        }
    }
    const auto offset = [&](const Luau::Position& position) {
        return line_starts.at(position.line) + position.column;
    };
    return source.content.substr(
        offset(location.begin),
        offset(location.end) - offset(location.begin)
    );
}

class ImportedFunctionCollector final : public Luau::AstVisitor {
  public:
    ImportedFunctionCollector(
        const LuauScriptSource& source,
        FunctionResolutionContext context,
        RuntimeFunctionCatalog& catalog
    ) : m_source(&source), m_context(context), m_catalog(&catalog) {}

    bool visit(AstExprIndexName* expression) override {
        const auto* module = expression->expr->as<AstExprLocal>();
        if (module == nullptr ||
            !m_context.module.imports().contains(module->local) ||
            !m_context.module_metadata_resolver) {
            return true;
        }
        auto declaration = compile_function_ref(
            *expression,
            m_context,
            "imported runtime function"
        );
        if (!declaration || m_names.contains(declaration->name)) {
            return true;
        }
        m_names.insert(declaration->name);
        m_catalog->plugin.functions.push_back(
            LuauFunctionDecl {
                .name = std::move(declaration->name),
                .params = std::move(declaration->params),
            }
        );
        m_catalog->expressions.push_back(
            expression_source(*m_source, expression->location)
        );
        return true;
    }

    void add_name(std::string_view name) { m_names.emplace(name); }

  private:
    const LuauScriptSource* m_source;
    // Callers construct this context inline, so the visitor must own it.
    FunctionResolutionContext m_context;
    RuntimeFunctionCatalog* m_catalog;
    std::unordered_set<std::string> m_names;
};

Result<RuntimeFunctionCatalog, LuauScriptError> compile_runtime_functions(
    const LuauScriptSource& source,
    Luau::AstStatBlock& root,
    const ModuleIR& module,
    const PluginIR& plugin,
    const LuauModuleMetadataResolver& module_metadata_resolver
) {
    RuntimeFunctionCatalog result {
        .plugin = LuauPluginDecl {
            .name = plugin.name,
            .source_name = source.name,
        },
    };
    ImportedFunctionCollector collector(
        source,
        FunctionResolutionContext {
            .module = module,
            .module_metadata_resolver = module_metadata_resolver,
        },
        result
    );
    for (const auto& function : module.functions()) {
        auto params = compile_function_params(*function.expression);
        if (!params) {
            continue;
        }
        result.plugin.functions.push_back(
            LuauFunctionDecl {
                .name = function.name,
                .params = std::move(*params),
            }
        );
        result.expressions.push_back(function.name);
        collector.add_name(function.name);
    }

    plugin.build->body->visit(&collector);
    return result;
}

} // namespace

Result<detail::luau_compiler::ModuleIR, LuauScriptError>
detail::luau_compiler::ModuleFrontendPass::run(
    const ParsedModule& parsed
) const {
    ModuleIR module;
    module.m_name = module_name_from_source(parsed.source().name);
    module.m_source_name = parsed.source().name;
    module.m_imports = collect_import_locals(parsed.root());

    module.m_functions = collect_top_level_functions(parsed.root());
    for (std::size_t index = 0; index < module.m_functions.size(); ++index) {
        module.m_function_lookup.emplace(
            module.m_functions[index].local,
            index
        );
    }

    auto plugins = find_exported_plugins(parsed.root());
    if (!plugins) {
        return failure(std::move(plugins.error()));
    }
    module.m_plugins = std::move(*plugins);
    std::unordered_map<const AstLocal*, std::string> local_plugins;
    for (const auto& plugin : module.m_plugins) {
        local_plugins.emplace(plugin.local, plugin.name);
    }
    for (auto& plugin : module.m_plugins) {
        auto dependencies = compile_plugin_dependencies(
            plugin,
            module.m_imports,
            local_plugins
        );
        if (!dependencies) {
            return failure(std::move(dependencies.error()));
        }
        plugin.dependencies = std::move(*dependencies);
    }

    auto types = compile_exported_types(parsed.root(), module.m_name);
    if (!types) {
        return failure(std::move(types.error()));
    }
    module.m_types = std::move(*types);
    for (std::size_t index = 0; index < module.m_types.size(); ++index) {
        const auto& type = module.m_types[index];
        module.m_type_lookup.emplace(type.name, index);
        module.m_type_lookup.emplace(type.qualified_name, index);
    }
    return module;
}

Result<LuauModuleMetadata, LuauScriptError>
detail::luau_compiler::ModuleMetadataPass::run(
    const LuauScriptSource& source,
    const Luau::AstStatBlock& root,
    const ModuleIR& module,
    LuauModuleSchema schema
) const {
    LuauModuleMetadata result {
        .source_hash = stable_name_hash(source.content),
        .schema = std::move(schema),
    };

    result.exported_types.reserve(module.types().size());
    for (const auto& type : module.types()) {
        const auto* unsupported = std::get_if<UnsupportedTypeIR>(&type.value);
        result.exported_types.push_back(
            LuauExportedTypeMetadata {
                .name = type.name,
                .runtime_compatible = unsupported == nullptr,
                .runtime_error = unsupported != nullptr ?
                                     unsupported->runtime_error :
                                     std::string {},
            }
        );
    }

    std::unordered_set<std::string> imports;
    for (const auto& [local, specifier] : module.imports()) {
        (void)local;
        if (imports.insert(specifier).second) {
            result.imports.push_back(specifier);
        }
    }
    std::ranges::sort(result.imports);

    std::vector<LuauFunctionDecl> compatible_functions;
    std::vector<std::size_t> compatible_metadata_indices;
    for (const auto& function : module.functions()) {
        if (!function.exported) {
            continue;
        }
        LuauFunctionMetadata metadata {
            .name = function.name,
            .qualified_name = module.name() + "." + function.name,
        };
        auto params = compile_function_params(*function.expression);
        if (!params) {
            metadata.system_signature_error = std::move(params.error().message);
        } else {
            compatible_metadata_indices.push_back(result.functions.size());
            compatible_functions.push_back(
                LuauFunctionDecl {
                    .name = function.name,
                    .params = std::move(*params),
                }
            );
        }
        result.functions.push_back(std::move(metadata));
    }

    TypeQualificationPass {}
        .run(source, root, result.schema, compatible_functions);
    for (std::size_t index = 0; index < compatible_functions.size(); ++index) {
        result.functions[compatible_metadata_indices[index]].system_params =
            std::move(compatible_functions[index].params);
    }

    result.plugins.reserve(module.plugins().size());
    for (const auto& plugin : module.plugins()) {
        LuauPluginMetadata metadata {.name = plugin.name};
        metadata.dependencies.reserve(plugin.dependencies.size());
        for (const auto& dependency : plugin.dependencies) {
            metadata.dependencies.push_back(
                LuauPluginDependency {
                    .import_specifier = dependency.import_specifier,
                    .plugin_name = dependency.plugin_name,
                }
            );
        }
        result.plugins.push_back(std::move(metadata));
    }
    return result;
}

bool is_native_luau_module(std::string_view specifier) {
    return is_luau_library(specifier) || specifier.starts_with("@entisium/");
}

Result<std::vector<std::string>, LuauScriptError>
extract_luau_script_imports(const LuauScriptSource& source) {
    enable_luau_language_features();
    detail::luau_compiler::CompilationSession session {source};
    auto parsed = session.parse();
    if (!parsed) {
        return failure(std::move(parsed.error()));
    }
    return extract_parsed_luau_script_imports(parsed->root());
}

Result<LuauModuleMetadata, LuauScriptError> compile_luau_module_metadata(
    const LuauScriptSource& source,
    bool snapshot_safe,
    bool allow_return
) {
    if (!enable_luau_language_features()) {
        return failure(
            LuauScriptError {"Luau value export feature flag is unavailable"}
        );
    }
    detail::luau_compiler::CompilationSession session {
        source,
        LuauCompileOptions {.snapshot_safe = snapshot_safe},
    };
    auto parsed = session.parse();
    if (!parsed) {
        return failure(declaration_error(std::move(parsed.error().message)));
    }
    auto& root = parsed->root();
    if (!allow_return &&
        std::ranges::any_of(root.body, [](const AstStat* statement) {
            return statement->is<AstStatReturn>();
        })) {
        return failure(
            declaration_error("top-level return declarations are not supported")
        );
    }
    if (snapshot_safe) {
        auto safe = validate_parsed_luau_snapshot_safety(source, root);
        if (!safe) {
            return failure(std::move(safe.error()));
        }
    }

    auto module_ir = detail::luau_compiler::ModuleFrontendPass {}.run(*parsed);
    if (!module_ir) {
        return failure(std::move(module_ir.error()));
    }
    auto runtime_type_uses =
        validate_local_runtime_type_uses(root, module_ir->types());
    if (!runtime_type_uses) {
        return failure(std::move(runtime_type_uses.error()));
    }
    auto lowered_module =
        detail::luau_compiler::ModuleSchemaLoweringPass {}.run(*module_ir);
    if (!lowered_module) {
        return failure(std::move(lowered_module.error()));
    }
    return detail::luau_compiler::ModuleMetadataPass {}
        .run(source, root, *module_ir, std::move(*lowered_module));
}

Result<LuauScriptModuleArtifact, LuauScriptError> compile_luau_script_module(
    const LuauScriptSource& source,
    LuauCompileOptions options
) {
    if (!enable_luau_language_features()) {
        return failure(
            LuauScriptError {"Luau value export feature flag is unavailable"}
        );
    }
    auto optimization_diagnostics =
        diagnose_luau_optimization_passes(options.optimization_passes);
    if (const auto invalid = std::ranges::find(
            optimization_diagnostics,
            LuauOptimizationDependencyKind::Required,
            &LuauOptimizationDiagnostic::kind
        );
        invalid != optimization_diagnostics.end()) {
        return failure(
            LuauScriptError {
                "Luau optimization pass '" +
                    std::string(luau_optimization_pass_name(invalid->pass)) +
                    "' requires '" +
                    std::string(
                        luau_optimization_pass_name(invalid->dependency)
                    ) +
                    "'",
            }
        );
    }
    detail::luau_compiler::CompilationSession session {
        source,
        std::move(options),
    };
    auto parsed = session.parse();
    if (!parsed) {
        return failure(declaration_error(std::move(parsed.error().message)));
    }
    auto& root = parsed->root();
    const auto& compile_options = session.options();
    const bool has_return =
        std::ranges::any_of(root.body, [](const AstStat* statement) {
            return statement->is<AstStatReturn>();
        });
    if (has_return && !compile_options.allow_return) {
        return failure(
            declaration_error("top-level return declarations are not supported")
        );
    }
    if (compile_options.snapshot_safe) {
        auto snapshot_safe = validate_parsed_luau_snapshot_safety(source, root);
        if (!snapshot_safe) {
            return failure(std::move(snapshot_safe.error()));
        }
    }

    auto module_ir = detail::luau_compiler::ModuleFrontendPass {}.run(*parsed);
    if (!module_ir) {
        return failure(std::move(module_ir.error()));
    }
    auto runtime_type_uses =
        validate_local_runtime_type_uses(root, module_ir->types());
    if (!runtime_type_uses) {
        return failure(std::move(runtime_type_uses.error()));
    }
    auto imported_runtime_type_uses = validate_imported_runtime_type_uses(
        root,
        *module_ir,
        compile_options.module_metadata_resolver
    );
    if (!imported_runtime_type_uses) {
        return failure(std::move(imported_runtime_type_uses.error()));
    }
    std::shared_ptr<const LuauModuleMetadata> metadata =
        compile_options.metadata;
    if (metadata) {
        if (metadata->source_hash != stable_name_hash(source.content) ||
            metadata->schema.source_name != source.name) {
            return failure(declaration_error(
                "cached Luau module metadata does not match source"
            ));
        }
    } else {
        auto lowered_module =
            detail::luau_compiler::ModuleSchemaLoweringPass {}.run(*module_ir);
        if (!lowered_module) {
            return failure(std::move(lowered_module.error()));
        }
        auto generated_metadata =
            detail::luau_compiler::ModuleMetadataPass {}
                .run(source, root, *module_ir, std::move(*lowered_module));
        if (!generated_metadata) {
            return failure(std::move(generated_metadata.error()));
        }
        metadata = std::make_shared<const LuauModuleMetadata>(
            std::move(*generated_metadata)
        );
    }
    LuauModuleSchema schema = metadata->schema;
    std::vector<LuauPluginDecl> plugins;
    std::vector<LuauFunctionDecl> functions;
    std::vector<LuauStateDecl> states;
    std::vector<std::string> runtime_expressions;
    std::unordered_set<std::string> function_names;
    std::unordered_set<std::string> state_names;
    std::unordered_set<TypeId> required_runtime_types;
    plugins.reserve(module_ir->plugins().size());
    for (const auto& plugin_ir : module_ir->plugins()) {
        auto runtime_functions = compile_runtime_functions(
            source,
            root,
            *module_ir,
            plugin_ir,
            compile_options.module_metadata_resolver
        );
        if (!runtime_functions) {
            return failure(std::move(runtime_functions.error()));
        }
        auto& plugin = runtime_functions->plugin;
        detail::luau_compiler::StateLoweringContext state_lowering {
            *module_ir,
            schema,
            plugin,
        };
        detail::luau_compiler::TypeQualificationPass {}
            .run(source, root, schema, plugin);
        auto state_usages = detail::luau_compiler::StateUsagePass {}.run(
            plugin,
            state_lowering
        );
        if (!state_usages) {
            return failure(std::move(state_usages.error()));
        }
        auto finalized_states = state_lowering.finalize();
        if (!finalized_states) {
            return failure(std::move(finalized_states.error()));
        }
        required_runtime_types.insert(
            state_lowering.required_runtime_types().begin(),
            state_lowering.required_runtime_types().end()
        );
        for (std::size_t index = 0; index < plugin.functions.size(); ++index) {
            const auto& function = plugin.functions[index];
            if (!function_names.insert(function.name).second) {
                continue;
            }
            functions.push_back(
                LuauFunctionDecl {
                    .name = function.name,
                    .params = clone_param_declarations(function.params),
                }
            );
            runtime_expressions.push_back(
                runtime_functions->expressions[index]
            );
        }
        for (const auto& state : plugin.states) {
            if (state_names.insert(state.qualified_name).second) {
                states.push_back(state);
            }
        }
        plugins.push_back(std::move(plugin));
    }

    std::vector<TypeId> required_types(
        required_runtime_types.begin(),
        required_runtime_types.end()
    );
    std::ranges::sort(required_types, {}, [](TypeId type) {
        return type.id();
    });
    auto lowered = detail::luau_compiler::PropertyLoweringPass {}.run(
        root,
        functions,
        compile_options.optimization_passes
    );
    auto generated =
        has_return ? lowered.source_patches.apply(source) :
                     detail::luau_compiler::RuntimeSourceEmissionPass {}.run(
                         source,
                         runtime_expressions,
                         std::move(lowered.source_patches)
                     );
    if (!generated) {
        return failure(std::move(generated.error()));
    }
    return LuauScriptModuleArtifact {
        .metadata = std::move(metadata),
        .plugins = std::move(plugins),
        .functions = std::move(functions),
        .states = std::move(states),
        .bytecode = Luau::compile(*generated),
        .required_runtime_types = std::move(required_types),
        .property_paths = std::move(lowered.property_paths),
        .optimization_report = std::move(lowered.optimization_report),
        .optimization_diagnostics = std::move(optimization_diagnostics),
    };
}

} // namespace ets
