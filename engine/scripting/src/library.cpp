#include "scripting/library.hpp"

#include <algorithm>
#include <filesystem>
#include <lua.h>
#include <lualib.h>
#include <Luau/Compiler.h>
#include <map>
#include <stdexcept>
#include <utility>

namespace ets {
namespace {
constexpr std::string_view internal_prefix = "@internal/";
std::string owner_name(std::string_view name) {
    return name.starts_with(internal_prefix) ?
               "@" + std::string(name.substr(internal_prefix.size())) :
               std::string(name);
}
std::string error_message(lua_State* state) {
    const char* text = lua_tostring(state, -1);
    return text ? text : "Library raised a non-string error";
}
} // namespace

bool is_luau_library(std::string_view name) {
    const auto owner = owner_name(name);
    return std::ranges::any_of(
        default_luau_libraries(),
        [&](const auto& entry) {
            return entry.name == owner &&
                   (!name.starts_with(internal_prefix) || entry.create);
        }
    );
}

struct LuauLibraries::Impl {
    struct LocalModule {
        int exports_ref {0};
        int thread_ref {0};
        bool loading {false};
    };
    struct Entry {
        LuauLibraryDefinition definition;
        std::unique_ptr<LuauNativeLibrary> instance;
        int native_ref {0};
        int exports_ref {0};
        int thread_ref {0};
        bool loading {false};
        std::map<std::string, LocalModule> modules;
    };
    lua_State* root;
    LuauLibraryServices services;
    std::map<std::string, Entry> entries;
    bool closed {false};

    static int require_callback(lua_State* state) {
        auto* self =
            static_cast<Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
        auto* importer =
            static_cast<Entry*>(lua_touserdata(state, lua_upvalueindex(2)));
        std::size_t size = 0;
        const char* name = luaL_checklstring(state, 1, &size);
        try {
            const std::string_view specifier(name, size);
            if (specifier.starts_with("./") || specifier.starts_with("../")) {
                self->load_local(
                    state,
                    specifier,
                    *importer,
                    lua_tostring(state, lua_upvalueindex(3))
                );
            } else {
                self->load(state, specifier, importer);
            }
            return 1;
        } catch (const std::exception& error) {
            lua_pushstring(state, error.what());
        }
        lua_error(state);
    }

    static int open_callback(lua_State* state) {
        auto* instance = static_cast<LuauNativeLibrary*>(
            lua_touserdata(state, lua_upvalueindex(1))
        );
        try {
            instance->open(state);
            if (lua_gettop(state) != 1 || !lua_istable(state, -1)) {
                throw std::runtime_error(
                    "Native library must return exactly one table"
                );
            }
            lua_setreadonly(state, -1, true);
            return 1;
        } catch (const std::exception& error) {
            lua_pushstring(state, error.what());
        }
        lua_error(state);
    }

    void
    install_require(lua_State* state, Entry& entry, std::string_view file) {
        lua_pushlightuserdata(state, this);
        lua_pushlightuserdata(state, &entry);
        lua_pushlstring(state, file.data(), file.size());
        lua_pushcclosure(state, require_callback, "library.require", 3);
        lua_setglobal(state, "require");
    }

    void release_local(Entry& entry) noexcept {
        for (auto& [name, module] : entry.modules) {
            if (module.exports_ref) {
                lua_unref(root, module.exports_ref);
            }
            if (module.thread_ref) {
                lua_unref(root, module.thread_ref);
            }
        }
        entry.modules.clear();
    }

    void load_local(
        lua_State* state,
        std::string_view name,
        Entry& entry,
        const char* importer
    ) {
        if (closed) {
            throw std::runtime_error("Luau libraries are closed");
        }
        const auto path = (std::filesystem::path(importer).parent_path() / name)
                              .lexically_normal();
        auto file = path.generic_string();
        if (path.is_absolute() || file.starts_with("../") || file == ".." ||
            name.find('\\') != std::string_view::npos ||
            name.find(':') != std::string_view::npos) {
            throw std::runtime_error("Library import escapes its directory");
        }
        if (!entry.definition.modules.contains(file)) {
            if (entry.definition.modules.contains(file + ".luau")) {
                file += ".luau";
            } else if (entry.definition.modules.contains(file + "/init.luau")) {
                file += "/init.luau";
            }
        }
        const auto source = entry.definition.modules.find(file);
        if (source == entry.definition.modules.end()) {
            throw std::runtime_error("Unknown library source: " + file);
        }
        auto& module = entry.modules[file];
        if (module.exports_ref) {
            lua_getref(state, module.exports_ref);
            return;
        }
        if (module.loading) {
            throw std::runtime_error("Circular library source import: " + file);
        }
        module.loading = true;
        const int top = lua_gettop(root);
        try {
            auto* thread = lua_newthread(root);
            module.thread_ref = lua_ref(root, -1);
            lua_pop(root, 1);
            luaL_sandboxthread(thread);
            install_require(thread, entry, file);
            const auto bytecode = Luau::compile(source->second);
            const auto chunk = entry.definition.name + "/" + file;
            if (luau_load(
                    thread,
                    chunk.c_str(),
                    bytecode.data(),
                    bytecode.size(),
                    0
                ) != 0 ||
                lua_pcall(thread, 0, 1, 0) != 0) {
                throw std::runtime_error(error_message(thread));
            }
            if (!lua_istable(thread, -1)) {
                throw std::runtime_error(
                    "Library source must return a table: " + file
                );
            }
            lua_setreadonly(thread, -1, true);
            module.exports_ref = lua_ref(thread, -1);
            lua_pop(thread, 1);
            module.loading = false;
        } catch (...) {
            if (module.thread_ref) {
                lua_unref(root, std::exchange(module.thread_ref, 0));
            }
            module.loading = false;
            lua_settop(root, top);
            throw;
        }
        lua_getref(state, module.exports_ref);
    }

    void load(lua_State* state, std::string_view name, Entry* importer) {
        if (closed) {
            throw std::runtime_error("Luau libraries are closed");
        }
        const bool internal = name.starts_with(internal_prefix);
        const auto owner = owner_name(name);
        const auto found = entries.find(owner);
        if (found == entries.end()) {
            throw std::runtime_error(
                "Unknown Luau library: " + std::string(name)
            );
        }
        auto& entry = found->second;
        if (internal &&
            (!importer ||
             (importer != &entry &&
              std::ranges::find(importer->definition.dependencies, name) ==
                  importer->definition.dependencies.end()))) {
            throw std::runtime_error(
                "Private Luau library: " + std::string(name)
            );
        }
        if (importer && importer != &entry &&
            std::ranges::find(importer->definition.dependencies, name) ==
                importer->definition.dependencies.end()) {
            throw std::runtime_error(
                "Undeclared Luau library dependency: " + std::string(name)
            );
        }
        if (internal && !entry.definition.create) {
            throw std::runtime_error("Library has no native exports: " + owner);
        }
        if ((internal ? entry.native_ref : entry.exports_ref) != 0) {
            lua_getref(state, internal ? entry.native_ref : entry.exports_ref);
            return;
        }
        if (entry.loading) {
            throw std::runtime_error(
                "Circular Luau library dependency: " + owner
            );
        }
        entry.loading = true;
        const int top = lua_gettop(root);
        try {
            auto* thread = lua_newthread(root);
            entry.thread_ref = lua_ref(root, -1);
            lua_pop(root, 1);
            luaL_sandboxthread(thread);
            install_require(thread, entry, "init.luau");
            for (const auto& dependency : entry.definition.dependencies) {
                load(thread, dependency, &entry);
                lua_pop(thread, 1);
            }
            if (entry.definition.create) {
                entry.instance = entry.definition.create(services);
                if (!entry.instance) {
                    throw std::runtime_error(
                        "Null native library instance: " + owner
                    );
                }
                lua_pushlightuserdata(thread, entry.instance.get());
                lua_pushcclosure(thread, open_callback, "library.open", 1);
                if (lua_pcall(thread, 0, 1, 0) != 0) {
                    throw std::runtime_error(error_message(thread));
                }
                entry.native_ref = lua_ref(thread, -1);
                lua_pop(thread, 1);
            }
            if (!entry.definition.source.empty()) {
                const auto bytecode = Luau::compile(entry.definition.source);
                if (luau_load(
                        thread,
                        owner.c_str(),
                        bytecode.data(),
                        bytecode.size(),
                        0
                    ) != 0 ||
                    lua_pcall(thread, 0, 1, 0) != 0) {
                    throw std::runtime_error(error_message(thread));
                }
                if (!lua_istable(thread, -1)) {
                    throw std::runtime_error(
                        "Library must return a table: " + owner
                    );
                }
            } else {
                if (!entry.native_ref) {
                    throw std::runtime_error(
                        "Library has no implementation: " + owner
                    );
                }
                lua_getref(thread, entry.native_ref);
            }
            lua_setreadonly(thread, -1, true);
            entry.exports_ref = lua_ref(thread, -1);
            lua_pop(thread, 1);
            entry.loading = false;
        } catch (...) {
            if (entry.instance) {
                entry.instance->cancel();
            }
            for (int* reference :
                 {&entry.native_ref, &entry.exports_ref, &entry.thread_ref}) {
                if (*reference) {
                    lua_unref(root, std::exchange(*reference, 0));
                }
            }
            release_local(entry);
            // Keep instances alive until VM destruction: an opener may have
            // allocated userdata whose finalizer still refers to its instance.
            if (entry.instance) {
                retired.push_back(std::move(entry.instance));
            }
            entry.loading = false;
            lua_settop(root, top);
            throw;
        }
        lua_getref(state, internal ? entry.native_ref : entry.exports_ref);
    }
    std::vector<std::unique_ptr<LuauNativeLibrary>> retired;
};

LuauLibraries::LuauLibraries(
    lua_State* root,
    LuauLibraryServices services,
    std::span<const LuauLibraryDefinition> definitions
) : m_impl(std::make_unique<Impl>()) {
    m_impl->root = root;
    m_impl->services = std::move(services);
    for (const auto& definition : definitions) {
        if (definition.name.empty() || definition.name.front() != '@' ||
            definition.name.starts_with(internal_prefix) ||
            !m_impl->entries
                 .try_emplace(
                     definition.name,
                     Impl::Entry {.definition = definition}
                 )
                 .second) {
            throw std::invalid_argument(
                "Invalid or duplicate Luau library: " + definition.name
            );
        }
    }
    std::map<std::string, int> marks;
    std::function<void(const std::string&)> visit = [&](
                                                        const std::string& name
                                                    ) {
        if (marks[name] == 1) {
            throw std::invalid_argument(
                "Circular Luau library dependency: " + name
            );
        }
        if (marks[name] == 2) {
            return;
        }
        const auto found = m_impl->entries.find(name);
        if (found == m_impl->entries.end()) {
            throw std::invalid_argument("Missing Luau library: " + name);
        }
        marks[name] = 1;
        for (const auto& dependency : found->second.definition.dependencies) {
            visit(owner_name(dependency));
            if (dependency.starts_with(internal_prefix) &&
                !m_impl->entries.at(owner_name(dependency)).definition.create) {
                throw std::invalid_argument(
                    "Missing native Luau library: " + dependency
                );
            }
        }
        marks[name] = 2;
    };
    for (const auto& [name, entry] : m_impl->entries) {
        visit(name);
    }
}
LuauLibraries::~LuauLibraries() = default;
bool LuauLibraries::contains(std::string_view name) const {
    return m_impl->entries.contains(owner_name(name));
}
void LuauLibraries::require(lua_State* state, std::string_view name) {
    m_impl->load(state, name, nullptr);
}
void LuauLibraries::enable_host_calls() {
    m_impl->services.host_calls = true;
}
void LuauLibraries::cancel() noexcept {
    for (auto& [name, entry] : m_impl->entries) {
        if (entry.instance) {
            entry.instance->cancel();
        }
    }
}
void LuauLibraries::close() noexcept {
    if (m_impl->closed) {
        return;
    }
    cancel();
    m_impl->closed = true;
    for (auto& [name, entry] : m_impl->entries) {
        for (int* reference :
             {&entry.native_ref, &entry.exports_ref, &entry.thread_ref}) {
            if (*reference) {
                lua_unref(m_impl->root, std::exchange(*reference, 0));
            }
        }
        m_impl->release_local(entry);
    }
}
std::size_t LuauLibraries::pending() const {
    std::size_t count = 0;
    for (const auto& [name, entry] : m_impl->entries) {
        if (entry.instance) {
            count += entry.instance->pending();
        }
    }
    return count;
}
std::string
LuauLibraries::diagnostic(std::string_view name, std::string_view key) const {
    const auto found = m_impl->entries.find(std::string(name));
    return found != m_impl->entries.end() && found->second.instance ?
               found->second.instance->diagnostic(key) :
               std::string {};
}
} // namespace ets
