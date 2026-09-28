#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <clang-c/Index.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace ets::reflgen {
namespace {

struct ReflectionMarker {
    std::size_t end_offset {0};
    std::vector<ReflectionAnnotation> annotations;
};

struct AnnotationMarker {
    std::size_t end_offset {0};
    std::string name;
};

struct DependentReturnMarker {
    std::size_t end_offset {0};
    std::string parameter;
};

struct TranslationUnitContext {
    CXTranslationUnit translation_unit = nullptr;
    std::string header_path;
    std::string comparable_header_path;
    std::string source;
    std::vector<ReflectionMarker> reflection_markers;
    std::vector<AnnotationMarker> annotation_markers;
    std::vector<DependentReturnMarker> dependent_return_markers;
    std::vector<AnnotationSchemaInfo> annotation_schemas;
};

[[nodiscard]] bool
cursor_is_from_header(CXCursor cursor, const TranslationUnitContext& context);

template<typename Visitor>
void visit_children(CXCursor cursor, Visitor visitor) {
    clang_visitChildren(
        cursor,
        [](CXCursor child, CXCursor parent, CXClientData data) {
            auto& fn = *static_cast<Visitor*>(data);
            return fn(child, parent);
        },
        &visitor
    );
}

[[nodiscard]] std::string clang_string(CXString value) {
    const char* cstr = clang_getCString(value);
    std::string result = cstr ? cstr : "";
    clang_disposeString(value);
    return result;
}

[[nodiscard]] std::string cursor_spelling(CXCursor cursor) {
    return clang_string(clang_getCursorSpelling(cursor));
}

[[nodiscard]] std::string cursor_display_name(CXCursor cursor) {
    return clang_string(clang_getCursorDisplayName(cursor));
}

[[nodiscard]] std::string type_spelling(CXType type) {
    return clang_string(clang_getTypeSpelling(type));
}

[[nodiscard]] bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) {
    return text.starts_with(prefix);
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
    return text.ends_with(suffix);
}

[[nodiscard]] std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

[[nodiscard]] bool is_type_id_parameter(std::string_view type_name) {
    auto type = trim(type_name);
    if (type.starts_with("const ")) {
        type.erase(0, 6);
    }
    while (type.ends_with('&')) {
        type.pop_back();
        type = trim(type);
    }
    if (type.ends_with('*')) {
        return false;
    }
    if (type.ends_with(" const")) {
        type.erase(type.size() - 6);
        type = trim(type);
    }
    return type == "TypeId" || type == "ets::TypeId";
}

[[nodiscard]] std::optional<std::size_t> cursor_offset(CXCursor cursor) {
    const auto location = clang_getRangeStart(clang_getCursorExtent(cursor));
    if (clang_equalLocations(location, clang_getNullLocation())) {
        return std::nullopt;
    }

    CXFile file = nullptr;
    unsigned offset = 0;
    clang_getExpansionLocation(location, &file, nullptr, nullptr, &offset);
    if (!file) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(offset);
}

[[nodiscard]] bool valid_annotation_identifier(std::string_view key) {
    return !key.empty() && std::ranges::all_of(key, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == ':' || ch == '.';
    });
}

[[nodiscard]] bool valid_annotation_value(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == ':' || ch == '.' ||
               ch == '-' || ch == '/' || ch == '@';
    });
}

// Skip one ordinary quoted string, keeping punctuation inside it opaque.
std::size_t quoted_end(std::string_view text, std::size_t begin) {
    for (std::size_t i = begin + 1; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
        } else if (text[i] == '"') {
            return i + 1;
        }
    }
    throw std::runtime_error("Unterminated ETS_REFLECT string literal");
}

[[nodiscard]] AnnotationArgument
parse_annotation_argument(std::string_view annotation, std::string_view text) {
    const auto separator = text.find('=');
    if (separator == std::string_view::npos) {
        const auto key = trim(text);
        if (!valid_annotation_identifier(key)) {
            throw std::runtime_error(
                "Invalid ETS_REFLECT annotation field '" + key + "'"
            );
        }
        return AnnotationArgument {
            .name = key,
            .value = "true",
        };
    }
    const auto key = trim(text.substr(0, separator));
    if (!valid_annotation_identifier(key)) {
        throw std::runtime_error(
            "Invalid ETS_REFLECT annotation field '" + key + "'"
        );
    }
    auto value = trim(text.substr(separator + 1));
    if (value.starts_with('"')) {
        std::string decoded;
        std::size_t position = 0;
        while (position < value.size()) {
            if (std::isspace(static_cast<unsigned char>(value[position]))) {
                ++position;
                continue;
            }
            if (value[position] != '"') {
                throw std::runtime_error(
                    "Expected adjacent annotation string literals"
                );
            }
            const auto end = quoted_end(value, position);
            decoded +=
                nlohmann::json::parse(value.substr(position, end - position))
                    .get<std::string>();
            position = end;
        }
        value = std::move(decoded);
    } else if (!valid_annotation_value(value)) {
        throw std::runtime_error(
            "Invalid value for ETS_REFLECT annotation '" +
            std::string(annotation) + "." + key + "'"
        );
    }
    return AnnotationArgument {
        .name = key,
        .value = std::move(value),
    };
}

template<typename Callback>
void for_each_top_level_item(std::string_view text, Callback callback) {
    std::size_t start = 0;
    int nesting = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        const bool at_end = index == text.size();
        const char character = at_end ? ',' : text[index];
        if (!at_end) {
            if (character == '"') {
                index = quoted_end(text, index) - 1;
                continue;
            }
            if (character == '(' || character == '<' || character == '[') {
                ++nesting;
            } else if (
                character == ')' || character == '>' || character == ']'
            ) {
                --nesting;
            }
        }

        if (nesting < 0) {
            throw std::runtime_error("ETS_REFLECT contains unmatched brackets");
        }

        if (character != ',' || nesting != 0) {
            continue;
        }

        auto item = trim(text.substr(start, index - start));
        if (!item.empty()) {
            callback(std::move(item));
        } else if (!trim(text).empty()) {
            throw std::runtime_error(
                "ETS_REFLECT contains an empty annotation"
            );
        }
        start = index + 1;
    }
    if (nesting != 0) {
        throw std::runtime_error("ETS_REFLECT contains unmatched brackets");
    }
}

[[nodiscard]] ReflectionAnnotation
parse_reflection_annotation(std::string_view text) {
    const auto group_begin = text.find('(');
    if (group_begin == std::string_view::npos) {
        if (text.find('=') != std::string_view::npos ||
            !valid_annotation_identifier(text)) {
            throw std::runtime_error(
                "Invalid ETS_REFLECT annotation '" + std::string(text) + "'"
            );
        }
        return ReflectionAnnotation {.name = std::string(text)};
    }

    const auto group = trim(text.substr(0, group_begin));
    if (!valid_annotation_identifier(group) || text.back() != ')') {
        throw std::runtime_error(
            "Invalid ETS_REFLECT annotation '" + std::string(text) + "'"
        );
    }

    ReflectionAnnotation annotation {.name = group};
    const auto fields =
        text.substr(group_begin + 1, text.size() - group_begin - 2);
    if (trim(fields).empty()) {
        throw std::runtime_error(
            "ETS_REFLECT annotation '" + group +
            "' must contain at least one field"
        );
    }
    for_each_top_level_item(fields, [&](std::string field) {
        annotation.arguments.push_back(parse_annotation_argument(group, field));
    });
    return annotation;
}

[[nodiscard]] std::vector<ReflectionAnnotation>
parse_reflection_annotations(std::string_view arguments) {
    std::vector<ReflectionAnnotation> annotations;
    for_each_top_level_item(arguments, [&](std::string item) {
        annotations.push_back(parse_reflection_annotation(item));
    });

    std::ranges::sort(annotations, {}, &ReflectionAnnotation::name);
    std::vector<ReflectionAnnotation> unique_annotations;
    for (auto& annotation : annotations) {
        if (unique_annotations.empty() ||
            unique_annotations.back().name != annotation.name) {
            unique_annotations.push_back(std::move(annotation));
        } else {
            auto& arguments = unique_annotations.back().arguments;
            arguments.insert(
                arguments.end(),
                std::make_move_iterator(annotation.arguments.begin()),
                std::make_move_iterator(annotation.arguments.end())
            );
        }
    }
    for (auto& annotation : unique_annotations) {
        std::ranges::sort(annotation.arguments, {}, &AnnotationArgument::name);
        auto unique_end = annotation.arguments.begin();
        for (auto argument = annotation.arguments.begin();
             argument != annotation.arguments.end();
             ++argument) {
            if (unique_end == annotation.arguments.begin() ||
                std::prev(unique_end)->name != argument->name) {
                if (unique_end != argument) {
                    *unique_end = std::move(*argument);
                }
                ++unique_end;
            } else if (std::prev(unique_end)->value != argument->value) {
                throw std::runtime_error(
                    "ETS_REFLECT annotation '" + annotation.name + "." +
                    argument->name + "' is declared with conflicting values"
                );
            }
        }
        annotation.arguments.erase(unique_end, annotation.arguments.end());
    }
    return unique_annotations;
}

[[nodiscard]] ReflectionMarker
parse_reflection_marker(std::string_view source, std::size_t marker_offset) {
    constexpr std::string_view c_marker_name = "ETS_REFLECT";
    std::size_t position = marker_offset + c_marker_name.size();
    while (position < source.size() &&
           std::isspace(static_cast<unsigned char>(source[position]))) {
        ++position;
    }
    if (position >= source.size() || source[position] != '(') {
        throw std::runtime_error(
            "ETS_REFLECT must be invoked with parentheses"
        );
    }

    const std::size_t arguments_begin = ++position;
    int depth = 1;
    while (position < source.size() && depth > 0) {
        if (source[position] == '"') {
            position = quoted_end(source, position);
            continue;
        }
        if (source[position] == '(') {
            ++depth;
        } else if (source[position] == ')') {
            --depth;
        }
        ++position;
    }
    if (depth != 0) {
        throw std::runtime_error("Unterminated ETS_REFLECT invocation");
    }

    const std::size_t arguments_end = position - 1;
    return ReflectionMarker {
        .end_offset = position,
        .annotations = parse_reflection_annotations(
            source.substr(arguments_begin, arguments_end - arguments_begin)
        ),
    };
}

[[nodiscard]] AnnotationMarker
parse_annotation_marker(std::string_view source, std::size_t marker_offset) {
    constexpr std::string_view c_marker_name = "ETS_ANNOTATION";
    std::size_t position = marker_offset + c_marker_name.size();
    while (position < source.size() &&
           std::isspace(static_cast<unsigned char>(source[position]))) {
        ++position;
    }
    if (position >= source.size() || source[position] != '(') {
        throw std::runtime_error(
            "ETS_ANNOTATION must be invoked with parentheses"
        );
    }

    const auto name_begin = ++position;
    const auto name_end = source.find(')', name_begin);
    if (name_end == std::string_view::npos) {
        throw std::runtime_error("Unterminated ETS_ANNOTATION invocation");
    }
    auto name = trim(source.substr(name_begin, name_end - name_begin));
    if (!valid_annotation_identifier(name) || name.contains('.') ||
        name.contains("::")) {
        throw std::runtime_error("Invalid ETS_ANNOTATION name '" + name + "'");
    }
    return AnnotationMarker {
        .end_offset = name_end + 1,
        .name = std::move(name),
    };
}

[[nodiscard]] DependentReturnMarker parse_dependent_return_marker(
    std::string_view source,
    std::size_t marker_offset
) {
    constexpr std::string_view c_marker_name = "ETS_DEPENDENT_RETURN";
    std::size_t position = marker_offset + c_marker_name.size();
    while (position < source.size() &&
           std::isspace(static_cast<unsigned char>(source[position]))) {
        ++position;
    }
    if (position >= source.size() || source[position] != '(') {
        throw std::runtime_error(
            "ETS_DEPENDENT_RETURN must be invoked with parentheses"
        );
    }

    const auto parameter_begin = ++position;
    const auto parameter_end = source.find(')', parameter_begin);
    if (parameter_end == std::string_view::npos) {
        throw std::runtime_error(
            "Unterminated ETS_DEPENDENT_RETURN invocation"
        );
    }
    auto parameter =
        trim(source.substr(parameter_begin, parameter_end - parameter_begin));
    if (parameter.empty() ||
        !std::ranges::all_of(
            parameter,
            [](const unsigned char character) {
                return std::isalnum(character) != 0 || character == '_';
            }
        ) ||
        std::isdigit(static_cast<unsigned char>(parameter.front())) != 0) {
        throw std::runtime_error(
            "Invalid ETS_DEPENDENT_RETURN parameter '" + parameter + "'"
        );
    }
    return DependentReturnMarker {
        .end_offset = parameter_end + 1,
        .parameter = std::move(parameter),
    };
}

[[nodiscard]] bool only_trivia_between(
    std::string_view source,
    std::size_t begin,
    std::size_t end
) {
    std::size_t position = begin;
    while (position < end) {
        if (std::isspace(static_cast<unsigned char>(source[position]))) {
            ++position;
            continue;
        }
        if (position + 1 < end && source[position] == '/' &&
            source[position + 1] == '/') {
            position += 2;
            while (position < end && source[position] != '\n') {
                ++position;
            }
            continue;
        }
        if (position + 1 < end && source[position] == '/' &&
            source[position + 1] == '*') {
            const auto comment_end = source.find("*/", position + 2);
            if (comment_end == std::string_view::npos ||
                comment_end + 2 > end) {
                return false;
            }
            position = comment_end + 2;
            continue;
        }
        return false;
    }
    return true;
}

void collect_reflection_markers(
    CXCursor cursor,
    TranslationUnitContext& context
) {
    visit_children(cursor, [&](CXCursor child, CXCursor) {
        if (clang_getCursorKind(child) == CXCursor_MacroExpansion &&
            cursor_is_from_header(child, context)) {
            if (const auto offset = cursor_offset(child)) {
                const auto macro = cursor_spelling(child);
                if (macro == "ETS_REFLECT") {
                    auto marker =
                        parse_reflection_marker(context.source, *offset);
                    context.reflection_markers.push_back(std::move(marker));
                } else if (macro == "ETS_ANNOTATION") {
                    auto marker =
                        parse_annotation_marker(context.source, *offset);
                    context.annotation_markers.push_back(std::move(marker));
                } else if (macro == "ETS_DEPENDENT_RETURN") {
                    auto marker =
                        parse_dependent_return_marker(context.source, *offset);
                    context.dependent_return_markers.push_back(
                        std::move(marker)
                    );
                }
            }
        }
        collect_reflection_markers(child, context);
        return CXChildVisit_Continue;
    });
}

[[nodiscard]] std::optional<std::string> dependent_return_parameter_for(
    CXCursor cursor,
    const TranslationUnitContext& context
) {
    const auto declaration_offset = cursor_offset(cursor);
    if (!declaration_offset) {
        return std::nullopt;
    }
    for (auto marker = context.dependent_return_markers.rbegin();
         marker != context.dependent_return_markers.rend();
         ++marker) {
        if (marker->end_offset > *declaration_offset) {
            continue;
        }
        if (only_trivia_between(
                context.source,
                marker->end_offset,
                *declaration_offset
            )) {
            return marker->parameter;
        }
        break;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<ReflectionAnnotation>>
reflection_annotations_for(
    CXCursor cursor,
    const TranslationUnitContext& context
) {
    const auto declaration_offset = cursor_offset(cursor);
    if (!declaration_offset) {
        return std::nullopt;
    }

    for (auto marker = context.reflection_markers.rbegin();
         marker != context.reflection_markers.rend();
         ++marker) {
        if (marker->end_offset > *declaration_offset) {
            continue;
        }
        if (only_trivia_between(
                context.source,
                marker->end_offset,
                *declaration_offset
            )) {
            return marker->annotations;
        }
        break;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> annotation_schema_name_for(
    CXCursor cursor,
    const TranslationUnitContext& context
) {
    const auto declaration_offset = cursor_offset(cursor);
    if (!declaration_offset) {
        return std::nullopt;
    }
    for (auto marker = context.annotation_markers.rbegin();
         marker != context.annotation_markers.rend();
         ++marker) {
        if (marker->end_offset > *declaration_offset) {
            continue;
        }
        if (only_trivia_between(
                context.source,
                marker->end_offset,
                *declaration_offset
            )) {
            return marker->name;
        }
        break;
    }
    return std::nullopt;
}

[[nodiscard]] std::filesystem::path
normalized_absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

[[nodiscard]] std::string
generic_absolute_path(const std::filesystem::path& path) {
    return normalized_absolute_path(path).generic_string();
}

[[nodiscard]] std::string comparable_path(std::string path) {
    std::ranges::replace(path, '\\', '/');
#ifdef _WIN32
    std::ranges::transform(path, path.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return path;
}

void insert_dependency(
    std::vector<std::string>& dependencies,
    const std::filesystem::path& dependency
) {
    auto normalized = generic_absolute_path(dependency);
    if (normalized.empty()) {
        return;
    }

    const auto comparable = comparable_path(normalized);
    const auto found = std::ranges::find_if(
        dependencies,
        [&comparable](const std::string& existing) {
            return comparable_path(existing) == comparable;
        }
    );
    if (found == dependencies.end()) {
        dependencies.push_back(std::move(normalized));
    }
}

void collect_translation_unit_dependencies(
    CXTranslationUnit translation_unit,
    std::vector<std::string>& dependencies
) {
    clang_getInclusions(
        translation_unit,
        [](CXFile included_file,
           CXSourceLocation*,
           unsigned,
           CXClientData data) {
            auto& deps = *static_cast<std::vector<std::string>*>(data);
            const auto filename =
                clang_string(clang_getFileName(included_file));
            if (!filename.empty()) {
                insert_dependency(deps, filename);
            }
        },
        &dependencies
    );
}

[[nodiscard]] std::optional<std::string> cursor_file_path(CXCursor cursor) {
    CXSourceLocation location = clang_getCursorLocation(cursor);
    if (clang_equalLocations(location, clang_getNullLocation())) {
        return std::nullopt;
    }

    CXFile file = nullptr;
    clang_getExpansionLocation(location, &file, nullptr, nullptr, nullptr);
    if (!file) {
        return std::nullopt;
    }

    const auto filename = clang_string(clang_getFileName(file));
    if (filename.empty()) {
        return std::nullopt;
    }
    return generic_absolute_path(filename);
}

[[nodiscard]] bool
cursor_is_from_header(CXCursor cursor, const TranslationUnitContext& context) {
    const auto file_path = cursor_file_path(cursor);
    if (!file_path) {
        return true;
    }
    return comparable_path(*file_path) == context.comparable_header_path;
}

[[nodiscard]] bool is_class_like(CXCursorKind kind) {
    return kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl;
}

[[nodiscard]] std::string
access_name(CX_CXXAccessSpecifier access, std::string_view fallback) {
    switch (access) {
        case CX_CXXPublic:
            return "public";
        case CX_CXXProtected:
            return "protected";
        case CX_CXXPrivate:
            return "private";
        case CX_CXXInvalidAccessSpecifier:
            break;
    }
    return std::string(fallback);
}

[[nodiscard]] std::string
cursor_access(CXCursor cursor, std::string_view fallback) {
    return access_name(clang_getCXXAccessSpecifier(cursor), fallback);
}

[[nodiscard]] std::string access_from_token_text(
    CXCursor cursor,
    std::string_view fallback,
    CXTranslationUnit translation_unit
) {
    CXToken* tokens = nullptr;
    unsigned token_count = 0;
    clang_tokenize(
        translation_unit,
        clang_getCursorExtent(cursor),
        &tokens,
        &token_count
    );

    std::string token_text;
    for (unsigned i = 0; i < token_count; ++i) {
        token_text +=
            clang_string(clang_getTokenSpelling(translation_unit, tokens[i]));
    }
    clang_disposeTokens(translation_unit, tokens, token_count);

    if (contains(token_text, "public")) {
        return "public";
    }
    if (contains(token_text, "protected")) {
        return "protected";
    }
    if (contains(token_text, "private")) {
        return "private";
    }
    return std::string(fallback);
}

[[nodiscard]] std::string qualified_name(CXCursor cursor) {
    if (clang_Cursor_isNull(cursor) ||
        clang_getCursorKind(cursor) == CXCursor_TranslationUnit) {
        return {};
    }

    const auto parent = clang_getCursorSemanticParent(cursor);
    std::string parent_name;
    if (!clang_Cursor_isNull(parent) &&
        clang_getCursorKind(parent) != CXCursor_TranslationUnit) {
        parent_name = qualified_name(parent);
    }

    auto current = cursor_spelling(cursor);
    if (current.empty()) {
        return parent_name;
    }
    if (parent_name.empty()) {
        return current;
    }
    return parent_name + "::" + current;
}

[[nodiscard]] std::vector<std::string> namespace_path(CXCursor cursor) {
    std::vector<std::string> result;
    auto parent = clang_getCursorSemanticParent(cursor);
    while (!clang_Cursor_isNull(parent) &&
           clang_getCursorKind(parent) != CXCursor_TranslationUnit) {
        if (clang_getCursorKind(parent) == CXCursor_Namespace) {
            auto name = cursor_spelling(parent);
            if (!name.empty()) {
                result.push_back(std::move(name));
            }
        }
        parent = clang_getCursorSemanticParent(parent);
    }
    std::ranges::reverse(result);
    return result;
}

[[nodiscard]] bool is_builtin_or_preserved_kind(CXTypeKind kind) {
    switch (kind) {
        case CXType_Void:
        case CXType_Bool:
        case CXType_Char_U:
        case CXType_UChar:
        case CXType_Char16:
        case CXType_Char32:
        case CXType_UShort:
        case CXType_UInt:
        case CXType_ULong:
        case CXType_ULongLong:
        case CXType_UInt128:
        case CXType_Char_S:
        case CXType_SChar:
        case CXType_WChar:
        case CXType_Short:
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
        case CXType_Int128:
        case CXType_Float:
        case CXType_Double:
        case CXType_LongDouble:
        case CXType_ConstantArray:
        case CXType_IncompleteArray:
        case CXType_Typedef:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool is_builtin_kind(CXTypeKind kind) {
    switch (kind) {
        case CXType_Void:
        case CXType_Bool:
        case CXType_Char_U:
        case CXType_UChar:
        case CXType_Char16:
        case CXType_Char32:
        case CXType_UShort:
        case CXType_UInt:
        case CXType_ULong:
        case CXType_ULongLong:
        case CXType_UInt128:
        case CXType_Char_S:
        case CXType_SChar:
        case CXType_WChar:
        case CXType_Short:
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
        case CXType_Int128:
        case CXType_Float:
        case CXType_Double:
        case CXType_LongDouble:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] std::string fully_qualified_type(CXType type);

[[nodiscard]] std::string format_function_pointer_type(CXType function_type) {
    auto return_type = fully_qualified_type(clang_getResultType(function_type));

    std::vector<std::string> param_types;
    const int arg_count = clang_getNumArgTypes(function_type);
    if (arg_count > 0) {
        param_types.reserve(static_cast<std::size_t>(arg_count));
    }
    for (int i = 0; i < arg_count; ++i) {
        param_types.push_back(fully_qualified_type(
            clang_getArgType(function_type, static_cast<unsigned>(i))
        ));
    }
    if (clang_isFunctionTypeVariadic(function_type)) {
        param_types.emplace_back("...");
    }

    std::string params;
    for (const auto& param_type : param_types) {
        if (!params.empty()) {
            params += ", ";
        }
        params += param_type;
    }
    return return_type + "(*)(" + params + ")";
}

[[nodiscard]] std::string fully_qualified_type(CXType type) {
    auto original_spelling = type_spelling(type);
    const auto canonical_type = clang_getCanonicalType(type);
    auto canonical_spelling = type_spelling(canonical_type);

    if (canonical_type.kind == CXType_Pointer) {
        const auto pointee = clang_getPointeeType(canonical_type);
        if (pointee.kind == CXType_FunctionProto ||
            pointee.kind == CXType_FunctionNoProto) {
            return format_function_pointer_type(pointee);
        }
        return fully_qualified_type(pointee) + "*";
    }

    if (canonical_type.kind == CXType_LValueReference ||
        canonical_type.kind == CXType_RValueReference) {
        const auto pointee = clang_getPointeeType(canonical_type);
        const auto pointee_type = fully_qualified_type(pointee);
        const auto ref_op =
            canonical_type.kind == CXType_LValueReference ? "&" : "&&";

        if ((pointee.kind == CXType_ConstantArray ||
             pointee.kind == CXType_IncompleteArray) &&
            contains(pointee_type, "[") && contains(pointee_type, "]")) {
            const auto array_start = pointee_type.find('[');
            return pointee_type.substr(0, array_start) + "(" + ref_op + ")" +
                   pointee_type.substr(array_start);
        }
        return pointee_type + ref_op;
    }

    if (is_builtin_or_preserved_kind(canonical_type.kind)) {
        return original_spelling;
    }

    if (type.kind == CXType_Typedef && is_builtin_kind(canonical_type.kind)) {
        return original_spelling;
    }

    if (!canonical_spelling.empty() && !original_spelling.empty() &&
        std::ranges::count(canonical_spelling, '<') >
            std::ranges::count(original_spelling, '<') &&
        std::ranges::count(canonical_spelling, '>') >
            std::ranges::count(original_spelling, '>')) {
        return canonical_spelling;
    }

    if (!original_spelling.empty() && contains(original_spelling, "::")) {
        return original_spelling;
    }

    const auto type_decl = clang_getTypeDeclaration(canonical_type);
    if (!clang_Cursor_isNull(type_decl) &&
        clang_getCursorKind(type_decl) != CXCursor_NoDeclFound) {
        auto qualified = qualified_name(type_decl);
        if (!qualified.empty()) {
            if (contains(original_spelling, "<") &&
                contains(original_spelling, ">")) {
                const auto template_start = original_spelling.find('<');
                const auto base_name = trim(
                    std::string_view(original_spelling)
                        .substr(0, template_start)
                );
                const auto qualified_base_pos = qualified.rfind("::");
                const auto qualified_base =
                    qualified_base_pos == std::string::npos ?
                        qualified :
                        qualified.substr(qualified_base_pos + 2);
                const auto template_part =
                    original_spelling.substr(template_start);

                if (base_name == qualified_base ||
                    ends_with(base_name, qualified_base)) {
                    return qualified + template_part;
                }
                if (contains(original_spelling, "::")) {
                    return original_spelling;
                }
                return qualified + template_part;
            }
            return qualified;
        }
    }

    return original_spelling.empty() ? canonical_spelling : original_spelling;
}

[[nodiscard]] std::string ref_qualifier(CXCursor cursor) {
    switch (clang_Type_getCXXRefQualifier(clang_getCursorType(cursor))) {
        case CXRefQualifier_LValue:
            return "&";
        case CXRefQualifier_RValue:
            return "&&";
        case CXRefQualifier_None:
            break;
    }

    const auto display_name = cursor_display_name(cursor);
    const auto right_paren = display_name.rfind(')');
    if (right_paren != std::string::npos) {
        auto suffix =
            trim(std::string_view(display_name).substr(right_paren + 1));
        if (starts_with(suffix, "const")) {
            suffix = trim(std::string_view(suffix).substr(5));
        }
        if (starts_with(suffix, "&&")) {
            return "&&";
        }
        if (starts_with(suffix, "&")) {
            return "&";
        }
    }
    return {};
}

[[nodiscard]] std::vector<ParamInfo> parameters_for(CXCursor cursor) {
    std::vector<ParamInfo> params;
    visit_children(cursor, [&](CXCursor child, CXCursor) {
        if (clang_getCursorKind(child) == CXCursor_ParmDecl) {
            params.push_back({
                .name = cursor_spelling(child),
                .type_name = fully_qualified_type(clang_getCursorType(child)),
            });
        }
        return CXChildVisit_Continue;
    });
    return params;
}

[[nodiscard]] std::optional<AnnotationSchemaInfo> parse_annotation_schema(
    CXCursor cursor,
    const TranslationUnitContext& context
) {
    if (!clang_isCursorDefinition(cursor)) {
        return std::nullopt;
    }
    const auto reflected_name = annotation_schema_name_for(cursor, context);
    if (!reflected_name) {
        return std::nullopt;
    }
    const auto source_file = cursor_file_path(cursor);
    if (!source_file) {
        return std::nullopt;
    }

    AnnotationSchemaInfo schema {
        .reflected_name = *reflected_name,
        .type_name = qualified_name(cursor),
        .source_file = *source_file,
    };
    std::string current_access(
        clang_getCursorKind(cursor) == CXCursor_ClassDecl ? "private" : "public"
    );
    visit_children(cursor, [&](CXCursor child, CXCursor) {
        const auto kind = clang_getCursorKind(child);
        if (kind == CXCursor_CXXAccessSpecifier) {
            current_access =
                access_name(clang_getCXXAccessSpecifier(child), current_access);
        } else if (kind == CXCursor_FieldDecl) {
            schema.fields.push_back({
                .name = cursor_spelling(child),
                .type_name = fully_qualified_type(clang_getCursorType(child)),
                .access = cursor_access(child, current_access),
            });
        }
        return CXChildVisit_Continue;
    });
    return schema;
}

void collect_annotation_schemas(
    CXCursor cursor,
    TranslationUnitContext& context
) {
    if (!cursor_is_from_header(cursor, context)) {
        return;
    }
    const auto kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) {
        if (auto schema = parse_annotation_schema(cursor, context)) {
            const auto duplicate = std::ranges::find_if(
                context.annotation_schemas,
                [&](const AnnotationSchemaInfo& existing) {
                    return existing.reflected_name == schema->reflected_name &&
                           existing.type_name == schema->type_name;
                }
            );
            if (duplicate == context.annotation_schemas.end()) {
                context.annotation_schemas.push_back(std::move(*schema));
            }
        }
    }
    visit_children(cursor, [&](CXCursor child, CXCursor) {
        collect_annotation_schemas(child, context);
        return CXChildVisit_Continue;
    });
}

[[nodiscard]] std::optional<EnumInfo>
parse_enum(CXCursor cursor, const TranslationUnitContext& context) {
    auto annotations = reflection_annotations_for(cursor, context);
    if (!clang_isCursorDefinition(cursor) || !annotations) {
        return std::nullopt;
    }

    auto enum_name = cursor_spelling(cursor);
    if (enum_name.empty()) {
        return std::nullopt;
    }

    auto enum_info = EnumInfo {
        .name = qualified_name(cursor),
        .namespace_path = namespace_path(cursor),
        .local_name = enum_name,
        .source_file = context.header_path,
        .annotations = std::move(*annotations),
        .underlying_type =
            fully_qualified_type(clang_getEnumDeclIntegerType(cursor)),
        .is_scoped = clang_EnumDecl_isScoped(cursor) != 0,
    };
    if (enum_info.name.empty()) {
        enum_info.name = std::move(enum_name);
    }

    visit_children(cursor, [&](CXCursor child, CXCursor) {
        if (clang_getCursorKind(child) == CXCursor_EnumConstantDecl) {
            enum_info.values.push_back({
                .name = cursor_spelling(child),
                .value = clang_getEnumConstantDeclValue(child),
            });
        }
        return CXChildVisit_Continue;
    });

    return enum_info;
}

[[nodiscard]] std::optional<ClassInfo> parse_class(
    CXCursor cursor,
    std::string_view default_access,
    const TranslationUnitContext& context
) {
    if (!clang_isCursorDefinition(cursor) ||
        clang_getCursorKind(cursor) == CXCursor_ClassTemplate) {
        return std::nullopt;
    }

    auto class_name = cursor_spelling(cursor);
    auto annotations = reflection_annotations_for(cursor, context);
    if (class_name.empty() || !annotations) {
        return std::nullopt;
    }

    auto qualified_class_name = qualified_name(cursor);
    if (qualified_class_name.empty()) {
        qualified_class_name = class_name;
    }

    const auto display_name = cursor_display_name(cursor);
    if (!display_name.empty() && display_name != class_name &&
        contains(display_name, "<")) {
        const auto scope_pos = qualified_class_name.rfind("::");
        if (scope_pos != std::string::npos) {
            qualified_class_name =
                qualified_class_name.substr(0, scope_pos + 2) + display_name;
        } else {
            qualified_class_name = display_name;
        }
    }

    ClassInfo class_info {
        .name = std::move(qualified_class_name),
        .namespace_path = namespace_path(cursor),
        .local_name = display_name.empty() ? class_name : display_name,
        .source_file = context.header_path,
        .annotations = std::move(*annotations),
    };

    std::string current_access(default_access);
    visit_children(cursor, [&](CXCursor child, CXCursor) {
        const auto kind = clang_getCursorKind(child);
        if (kind == CXCursor_CXXAccessSpecifier) {
            current_access = access_from_token_text(
                child,
                current_access,
                context.translation_unit
            );
            return CXChildVisit_Continue;
        }

        const auto member_access = cursor_access(child, current_access);
        const bool library_method =
            member_access == "public" &&
            has_annotation(class_info.annotations, "LuauLibrary") &&
            std::ranges::none_of(class_info.annotations, [](const auto& mark) {
                return mark.name == "LuauLibrary" &&
                       std::ranges::any_of(
                           mark.arguments,
                           [](const auto& argument) {
                               return argument.name == "custom" &&
                                      argument.value == "true";
                           }
                       );
            });
        if (library_method && kind == CXCursor_FunctionTemplate) {
            throw std::runtime_error(
                "Luau library method templates require an adapter: " +
                class_name
            );
        }
        if (kind == CXCursor_FieldDecl) {
            const auto field_type = clang_getCursorType(child);
            auto property_type = fully_qualified_type(field_type);
            if (!property_type.empty()) {
                class_info.properties.push_back({
                    .name = cursor_spelling(child),
                    .type_name = std::move(property_type),
                    .access = member_access,
                });
            }
        } else if (kind == CXCursor_CXXMethod) {
            if (library_method && (clang_Cursor_isVariadic(child) ||
                                   clang_CXXMethod_isDeleted(child) ||
                                   !ref_qualifier(child).empty())) {
                throw std::runtime_error(
                    "Unsupported Luau library method: " + class_name + "." +
                    cursor_spelling(child)
                );
            }
            if (clang_CXXMethod_isDeleted(child)) {
                return CXChildVisit_Continue;
            }

            auto qualifier = ref_qualifier(child);
            if (!qualifier.empty()) {
                return CXChildVisit_Continue;
            }

            MethodInfo method;
            method.annotations =
                reflection_annotations_for(child, context)
                    .value_or(std::vector<ReflectionAnnotation> {});
            method.name = cursor_spelling(child);
            method.type_name =
                fully_qualified_type(clang_getCursorResultType(child));
            method.access = member_access;
            method.parameters = parameters_for(child);
            method.dependent_return_parameter =
                dependent_return_parameter_for(child, context);
            if (method.dependent_return_parameter) {
                const auto parameter = std::ranges::find(
                    method.parameters,
                    *method.dependent_return_parameter,
                    &ParamInfo::name
                );
                if (parameter == method.parameters.end()) {
                    throw std::runtime_error(
                        "ETS_DEPENDENT_RETURN on method '" + method.name +
                        "' names missing parameter '" +
                        *method.dependent_return_parameter + "'"
                    );
                }
                if (!is_type_id_parameter(parameter->type_name)) {
                    throw std::runtime_error(
                        "ETS_DEPENDENT_RETURN parameter '" +
                        *method.dependent_return_parameter + "' on method '" +
                        method.name + "' must have type ets::TypeId"
                    );
                }
            }
            method.is_static = clang_CXXMethod_isStatic(child) != 0;
            method.is_const = clang_CXXMethod_isConst(child) != 0;
            method.ref_qualifier = std::move(qualifier);
            method.is_abstract = clang_CXXMethod_isPureVirtual(child) != 0;
            class_info.methods.push_back(std::move(method));
        } else if (kind == CXCursor_Constructor) {
            if (clang_CXXMethod_isDeleted(child)) {
                return CXChildVisit_Continue;
            }

            MethodInfo constructor;
            constructor.name = class_info.name;
            constructor.type_name = class_info.name;
            constructor.access = member_access;
            constructor.parameters = parameters_for(child);
            constructor.is_converting =
                constructor.parameters.size() == 1 &&
                clang_CXXConstructor_isConvertingConstructor(child) != 0;
            class_info.constructors.push_back(std::move(constructor));
        }

        return CXChildVisit_Continue;
    });

    return class_info;
}

[[nodiscard]] ParseResult parse_cursor(
    CXCursor cursor,
    const TranslationUnitContext& context,
    std::string_view current_access = "private",
    CXCursorKind parent_kind = CXCursor_NoDeclFound
) {
    ParseResult result;
    if (!cursor_is_from_header(cursor, context)) {
        return result;
    }

    const auto kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) {
        if (is_class_like(parent_kind) &&
            (current_access == "private" || current_access == "protected")) {
            return result;
        }

        if (auto class_info = parse_class(
                cursor,
                kind == CXCursor_ClassDecl ? "private" : "public",
                context
            )) {
            result.classes.push_back(std::move(*class_info));
        }
    } else if (kind == CXCursor_EnumDecl) {
        if (is_class_like(parent_kind) &&
            (current_access == "private" || current_access == "protected")) {
            return result;
        }

        if (auto enum_info = parse_enum(cursor, context)) {
            result.enums.push_back(std::move(*enum_info));
        }
    }

    if (kind == CXCursor_ClassTemplate) {
        return result;
    }

    if (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) {
        std::string child_access =
            kind == CXCursor_ClassDecl ? "private" : "public";
        visit_children(cursor, [&](CXCursor child, CXCursor) {
            if (clang_getCursorKind(child) == CXCursor_CXXAccessSpecifier) {
                child_access = access_from_token_text(
                    child,
                    child_access,
                    context.translation_unit
                );
            } else {
                auto child_result =
                    parse_cursor(child, context, child_access, kind);
                result.classes.insert(
                    result.classes.end(),
                    std::make_move_iterator(child_result.classes.begin()),
                    std::make_move_iterator(child_result.classes.end())
                );
                result.enums.insert(
                    result.enums.end(),
                    std::make_move_iterator(child_result.enums.begin()),
                    std::make_move_iterator(child_result.enums.end())
                );
            }
            return CXChildVisit_Continue;
        });
    } else {
        visit_children(cursor, [&](CXCursor child, CXCursor) {
            auto child_result =
                parse_cursor(child, context, current_access, kind);
            result.classes.insert(
                result.classes.end(),
                std::make_move_iterator(child_result.classes.begin()),
                std::make_move_iterator(child_result.classes.end())
            );
            result.enums.insert(
                result.enums.end(),
                std::make_move_iterator(child_result.enums.begin()),
                std::make_move_iterator(child_result.enums.end())
            );
            return CXChildVisit_Continue;
        });
    }

    return result;
}

[[nodiscard]] HeaderParseOutput parse_header(
    const std::string& header,
    const std::vector<std::string>& include_paths,
    bool verbose
) {
    HeaderParseOutput output;
    const auto header_path = generic_absolute_path(header);
    insert_dependency(output.dependencies, header_path);
    if (!std::filesystem::exists(header_path)) {
        std::cerr << "Warning: Header file '" << header << "' not found\n";
        return output;
    }

    std::ifstream source_file(header_path, std::ios::binary);
    if (!source_file) {
        std::cerr << "Warning: Failed to read " << header << '\n';
        return output;
    }
    std::string source {
        std::istreambuf_iterator<char> {source_file},
        std::istreambuf_iterator<char> {}
    };

    CXIndex index = clang_createIndex(0, 0);
    std::vector<std::string> args =
        {"-x", "c++-header", "-std=c++23", "-DETS_REFLGEN_SCRIPT"};
    for (const auto& include_path : include_paths) {
        args.emplace_back("-I");
        args.push_back(include_path);
    }

    std::vector<const char*> c_args;
    c_args.reserve(args.size());
    for (const auto& arg : args) {
        c_args.push_back(arg.c_str());
    }

    CXTranslationUnit translation_unit = clang_parseTranslationUnit(
        index,
        header_path.c_str(),
        c_args.data(),
        static_cast<int>(c_args.size()),
        nullptr,
        0,
        CXTranslationUnit_DetailedPreprocessingRecord
    );

    if (!translation_unit) {
        clang_disposeIndex(index);
        std::cerr << "Warning: Failed to parse " << header << '\n';
        return output;
    }

    const unsigned diagnostic_count = clang_getNumDiagnostics(translation_unit);
    for (unsigned i = 0; i < diagnostic_count; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(translation_unit, i);
        const auto severity = clang_getDiagnosticSeverity(diagnostic);
        output.has_errors = output.has_errors || severity >= CXDiagnostic_Error;
        if (severity >= CXDiagnostic_Warning) {
            std::cerr << "Warning in " << header << ": "
                      << clang_string(clang_getDiagnosticSpelling(diagnostic))
                      << '\n';
        } else if (verbose) {
            std::cerr << "Note in " << header << ": "
                      << clang_string(clang_getDiagnosticSpelling(diagnostic))
                      << '\n';
        }
        clang_disposeDiagnostic(diagnostic);
    }

    TranslationUnitContext context {
        .translation_unit = translation_unit,
        .header_path = header_path,
        .comparable_header_path = comparable_path(header_path),
        .source = std::move(source),
    };

    collect_reflection_markers(
        clang_getTranslationUnitCursor(translation_unit),
        context
    );
    std::ranges::sort(
        context.annotation_markers,
        {},
        &AnnotationMarker::end_offset
    );
    collect_annotation_schemas(
        clang_getTranslationUnitCursor(translation_unit),
        context
    );
    std::ranges::sort(
        context.reflection_markers,
        {},
        &ReflectionMarker::end_offset
    );

    collect_translation_unit_dependencies(
        translation_unit,
        output.dependencies
    );

    output.result =
        parse_cursor(clang_getTranslationUnitCursor(translation_unit), context);
    for (const auto& schema : context.annotation_schemas) {
        if (comparable_path(schema.source_file) ==
            context.comparable_header_path) {
            output.result.annotation_schemas.push_back(schema);
        }
    }
    std::ranges::sort(output.dependencies);

    clang_disposeTranslationUnit(translation_unit);
    clang_disposeIndex(index);
    return output;
}

} // namespace

HeaderParser::HeaderParser(
    std::vector<std::string> headers,
    std::vector<std::string> include_paths,
    bool verbose
) :
    m_headers(std::move(headers)), m_include_paths(std::move(include_paths)),
    m_verbose(verbose) {}

HeaderParseOutput HeaderParser::parse() {
    HeaderParseOutput output;
    for (const auto& header : m_headers) {
        auto header_result = parse_header(header, m_include_paths, m_verbose);
        output.has_errors = output.has_errors || header_result.has_errors;
        output.result.annotation_schemas.insert(
            output.result.annotation_schemas.end(),
            std::make_move_iterator(
                header_result.result.annotation_schemas.begin()
            ),
            std::make_move_iterator(
                header_result.result.annotation_schemas.end()
            )
        );
        output.result.classes.insert(
            output.result.classes.end(),
            std::make_move_iterator(header_result.result.classes.begin()),
            std::make_move_iterator(header_result.result.classes.end())
        );
        output.result.enums.insert(
            output.result.enums.end(),
            std::make_move_iterator(header_result.result.enums.begin()),
            std::make_move_iterator(header_result.result.enums.end())
        );
        for (const auto& dependency : header_result.dependencies) {
            insert_dependency(output.dependencies, dependency);
        }
    }
    std::ranges::sort(output.dependencies);
    return output;
}

} // namespace ets::reflgen
