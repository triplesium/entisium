#include "emitter.hpp"
#include "manifest.hpp"
#include "type_mapper.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp> // IWYU pragma: keep
#include <string>

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        m_path =
            std::filesystem::temp_directory_path() /
            ("entisium-luau-defgen-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()
             ));
        std::filesystem::create_directories(m_path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
};

void write(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

[[nodiscard]] std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char> {input},
        std::istreambuf_iterator<char> {},
    };
}

} // namespace

TEST_CASE(
    "defgen maps reflected and primitive C++ types",
    "[luau-defgen][types]"
) {
    ets::luau_defgen::Database database;
    database.classes.push_back({.cpp_name = "ets::Vector2", .name = "Vector2"});
    database.classes.push_back(
        {.cpp_name = "ets::AssetLoadError", .name = "AssetLoadError"}
    );
    database.classes.push_back({
        .cpp_name = "ets::AssetPath",
        .name = "AssetPath",
        .constructors = {{
            .parameters = {{
                .name = "path",
                .cpp_type = "const std::string&",
            }},
            .converting = true,
        }},
    });
    database.classes.push_back({
        .cpp_name = "ets::ExplicitPath",
        .name = "ExplicitPath",
        .constructors = {{
            .parameters = {{
                .name = "path",
                .cpp_type = "const std::string&",
            }},
            .converting = false,
        }},
    });
    ets::luau_defgen::TypeMapper mapper {database};

    CHECK(mapper.map("bool") == "boolean");
    CHECK(mapper.map("const std::basic_string<char>&") == "string");
    CHECK(mapper.map("std::uint32_t") == "number");
    CHECK(mapper.map("ets::TypeId") == "TypeToken<any>");
    CHECK(mapper.map("const ets::Vector2&") == "Vector2");
    CHECK(mapper.map("const ets::Vector2*") == "Vector2?");
    CHECK(mapper.map("ets::Optional<AssetLoadError>") == "AssetLoadError?");
    CHECK(mapper.map("std::optional<const ets::Vector2*>") == "Vector2?");
    CHECK(
        mapper.map_parameter("const ets::AssetPath&") == "AssetPath | string"
    );
    CHECK(mapper.map_parameter("const ets::ExplicitPath&") == "ExplicitPath");
    CHECK(
        mapper.map_dependent_return(
            "ets::Result<ets::UntypedHandle, ets::AssetTypeError>"
        ) == "Handle<T>"
    );
    CHECK(
        mapper.map_dependent_return(
            "ets::Result<ets::Optional<ets::UntypedHandle>, Error>"
        ) == "Handle<T>?"
    );
    CHECK_THROWS(mapper.map_dependent_return(
        "ets::Result<std::pair<ets::UntypedHandle, "
        "ets::UntypedHandle>, Error>"
    ));
    CHECK_THROWS(mapper.map_dependent_return("int"));
    CHECK(mapper.map("std::vector<float>") == "ReflectedSequence<number>");
    CHECK(
        mapper.map("std::map<std::string, std::vector<int>>") ==
        "ReflectedMap<string, ReflectedSequence<number>>"
    );
    CHECK(
        mapper.map_parameter("const std::array<float, 3>&") ==
        "ReflectedSequence<number>"
    );
    CHECK(
        mapper.map_library_parameter("const std::array<float, 3>&") ==
        "ReflectedSequence<number> | {number}"
    );
    CHECK(
        mapper.map_parameter("std::vector<int>") == "ReflectedSequence<number>"
    );
    CHECK(
        mapper.map_library_parameter("std::vector<int>") ==
        "ReflectedSequence<number> | {number}"
    );
    CHECK(
        mapper.map_parameter("std::map<std::string, int>") ==
        "ReflectedMap<string, number>"
    );
    CHECK(
        mapper.map_library_parameter("std::map<std::string, int>") ==
        "ReflectedMap<string, number> | {[string]: number}"
    );
    CHECK(
        mapper.map_library_parameter("std::optional<std::vector<int>>") ==
        "(ReflectedSequence<number> | {number})?"
    );
    CHECK_FALSE(mapper.unsupported_types().contains("std::vector<float>"));
}

TEST_CASE(
    "defgen emits global and native module definitions",
    "[luau-defgen][emit]"
) {
    TemporaryDirectory temporary;
    const auto manifest = temporary.path() / "vector.refl.json";
    const auto manual = temporary.path() / "runtime.d.luau";
    const auto output = temporary.path() / "out";
    write(manual, "export type entity = number\n");
    const auto sources = temporary.path() / "sources";
    const auto runtime = sources / "scripting";
    const auto libraries = sources / "libraries";
    for (const auto* path :
         {"libraries/task/task.luau",
          "libraries/schema/schema.luau",
          "libraries/context/context.luau",
          "scripting/libraries/playtest/game.luau",
          "scripting/libraries/playtest/scheduler.luau",
          "scripting/libraries/playtest/context.luau",
          "libraries/ai/ai.luau",
          "libraries/http/http.luau",
          "libraries/json/json.luau"}) {
        write(
            sources / path,
            std::string("--!strict\n-- ") + path + "\nreturn {}\n"
        );
    }
    write(output / "modules" / "stale.luau", "return {}\n");
    write(
        manifest,
        R"({
  "format": "entisium.reflection",
  "version": 1,
  "module": "math",
  "annotationSchemas": [],
  "classes": [{
    "cppName": "ets::Vector2",
    "name": "Vector2",
    "namespace": ["ets"],
    "source": "engine/math/vector.hpp",
    "abstract": false,
    "annotations": [{"name": "ScriptPrelude", "arguments": []}],
    "properties": [{"name": "x", "cppType": "float"}],
    "methods": [{"name": "length", "returnCppType": "float", "parameters": [], "static": false, "const": true}],
    "constructors": [
      {"parameters": [{"name": "x", "cppType": "float"}]},
      {"parameters": [{"name": "value", "cppType": "double"}]}
    ]
  }],
  "enums": []
})"
    );

    nlohmann::ordered_json library_entries = nlohmann::ordered_json::array();
    for (const auto& [name, file] :
         std::vector<std::pair<std::string, std::string>> {
             {"@task", "task/task.luau"},
             {"@schema", "schema/schema.luau"},
             {"@context/core", "context/context.luau"},
             {"@ai", "ai/ai.luau"},
             {"@http", "http/http.luau"},
             {"@json", "json/json.luau"}
         }) {
        library_entries.push_back(
            {{"name", name},
             {"source", read(libraries / file)},
             {"native", ""},
             {"globals",
              name == "@json" ?
                  "declare extern type SelectedJsonNull with\nend" :
                  ""},
             {"dependencies", nlohmann::json::array()}}
        );
    }
    const auto library_manifest = sources / "libraries.json";
    write(
        library_manifest,
        nlohmann::ordered_json {
            {"format", "entisium.luau-libraries"},
            {"version", 1},
            {"libraries", library_entries}
        }.dump()
    );
    const std::vector manifests {manifest};
    const auto database = ets::luau_defgen::load_manifests(manifests);
    write(output / "playtest" / "test.luau", "stale test facade");
    const auto summary = ets::luau_defgen::emit_definitions(
        database,
        manual,
        output,
        runtime,
        library_manifest
    );

    CHECK_FALSE(std::filesystem::exists(output / "playtest" / "test.luau"));
    CHECK(summary.class_count == 1);
    const auto globals = read(output / "globals.d.luau");
    CHECK(
        globals.find("declare extern type __Entisium_ets_Vector2") !=
        std::string::npos
    );
    CHECK(
        globals.find("export type Vector2 = __Entisium_ets_Vector2") !=
        std::string::npos
    );
    CHECK(globals.find("declare Vector2: {") != std::string::npos);
    CHECK(globals.find("new: ((x: number) -> Vector2)") != std::string::npos);
    CHECK(globals.find("value: number") == std::string::npos);
    CHECK(globals.find("__ets_type: Vector2?") != std::string::npos);
    CHECK(globals.find("__ets_type_id") == std::string::npos);
    CHECK(globals.find("__ets_type_name") == std::string::npos);
    CHECK(globals.find("__type_id") == std::string::npos);
    CHECK(globals.find("__type_name") == std::string::npos);
    const auto module = read(output / "modules" / "math.luau");
    CHECK(
        module.find("export type Vector2 = __Entisium_ets_Vector2") !=
        std::string::npos
    );
    CHECK(module.find("Vector2 = Vector2") != std::string::npos);
    CHECK(read(output / ".luaurc").find("\"entisium\"") != std::string::npos);
    CHECK(
        read(output / ".luaurc").find("\"schema\": \"./schema\"") !=
        std::string::npos
    );
    for (const auto& [source, destination] :
         std::vector<std::pair<std::string, std::string>> {
             {"libraries/task/task.luau", "task/init.luau"},
             {"libraries/schema/schema.luau", "schema/init.luau"},
             {"libraries/context/context.luau", "context/core.luau"},
             {"scripting/libraries/playtest/context.luau", "context/init.luau"},
             {"scripting/libraries/playtest/game.luau", "playtest/game.luau"},
             {"scripting/libraries/playtest/scheduler.luau",
              "playtest/scheduler.luau"},
             {"libraries/ai/ai.luau", "ai/init.luau"},
             {"libraries/http/http.luau", "http/init.luau"},
             {"libraries/json/json.luau", "json/init.luau"},
         }) {
        CHECK(read(output / destination) == read(sources / source));
    }
    CHECK_FALSE(std::filesystem::exists(output / "modules" / "stale.luau"));
    CHECK(read(output / ".luaurc").find("\"task\"") != std::string::npos);
    CHECK(
        read(output / "globals.d.luau").find("SelectedJsonNull") !=
        std::string::npos
    );
    CHECK(
        read(output / "json/init.luau").find("SelectedJsonNull") ==
        std::string::npos
    );
    const std::string native_contract =
        "return nil :: any -- native-only contract\n";
    write(
        library_manifest,
        nlohmann::ordered_json {
            {"format", "entisium.luau-libraries"},
            {"version", 1},
            {"libraries",
             nlohmann::ordered_json::array(
                 {{{"name", "@native"},
                   {"source", ""},
                   {"native", native_contract},
                   {"modules", {{"helper.luau", "return {value = 42}"}}}}}
             )}
        }.dump()
    );
    write(output / "user.luau", "preserved");
    const auto native_summary = ets::luau_defgen::emit_definitions(
        database,
        manual,
        output,
        runtime,
        library_manifest
    );
    CHECK(native_summary.class_count == summary.class_count);
    CHECK(
        read(output / "globals.d.luau").find("SelectedJsonNull") ==
        std::string::npos
    );
    CHECK(read(output / "native/init.luau") == native_contract);
    CHECK(read(output / "internal/native.luau") == native_contract);
    CHECK(read(output / "native/helper.luau") == "return {value = 42}");
    CHECK_FALSE(std::filesystem::exists(output / "json/init.luau"));
    CHECK(read(output / "user.luau") == "preserved");
    CHECK(read(output / ".luaurc").find("\"task\"") == std::string::npos);
}

TEST_CASE(
    "defgen removes overloads that collapse to the same Luau signature",
    "[luau-defgen][emit][overload]"
) {
    TemporaryDirectory temporary;
    const auto manifest = temporary.path() / "asset.refl.json";
    const auto manual = temporary.path() / "runtime.d.luau";
    const auto output = temporary.path() / "out";
    write(manual, "");
    const auto sources = temporary.path() / "sources";
    const auto runtime = sources / "scripting";
    const auto libraries = sources / "libraries";
    for (const auto* path :
         {"libraries/task/task.luau",
          "libraries/schema/schema.luau",
          "libraries/context/context.luau",
          "scripting/libraries/playtest/game.luau",
          "scripting/libraries/playtest/scheduler.luau",
          "scripting/libraries/playtest/context.luau",
          "libraries/ai/ai.luau",
          "libraries/http/http.luau",
          "libraries/json/json.luau"}) {
        write(sources / path, "--!strict\nreturn {}\n");
    }
    write(
        manifest,
        R"({
  "format": "entisium.reflection",
  "version": 1,
  "module": "asset",
  "annotationSchemas": [],
  "classes": [{
    "cppName": "ets::AssetLoadError",
    "name": "AssetLoadError",
    "namespace": ["ets"],
    "source": "engine/asset/loader.hpp",
    "abstract": false,
    "annotations": [],
    "properties": [{"name": "message", "cppType": "std::string"}],
    "methods": [],
    "constructors": []
  }, {
    "cppName": "ets::AssetPath",
    "name": "AssetPath",
    "namespace": ["ets"],
    "source": "engine/asset/path.hpp",
    "abstract": false,
    "annotations": [],
    "properties": [],
    "methods": [],
    "constructors": [{"parameters": [{"name": "path", "cppType": "const std::string&"}], "converting": true}]
  }, {
    "cppName": "ets::AssetServer",
    "name": "AssetServer",
    "namespace": ["ets"],
    "source": "engine/asset/asset_server.hpp",
    "abstract": false,
    "annotations": [],
    "properties": [],
    "methods": [
      {"name": "load", "returnCppType": "ets::Result<ets::UntypedHandle, ets::AssetTypeError>", "parameters": [{"name": "type", "cppType": "ets::TypeId"}, {"name": "path", "cppType": "const ets::AssetPath&"}], "dependentReturnParameter": "type", "static": false, "const": false},
      {"name": "load_error", "returnCppType": "ets::Optional<AssetLoadError>", "parameters": [{"name": "key", "cppType": "ets::AssetKey"}], "static": false, "const": true},
      {"name": "load_error", "returnCppType": "ets::Optional<AssetLoadError>", "parameters": [{"name": "handle", "cppType": "const ets::UntypedHandle&"}], "static": false, "const": true}
    ],
    "constructors": []
  }],
  "enums": []
})"
    );

    nlohmann::ordered_json library_entries = nlohmann::ordered_json::array();
    for (const auto& [name, file] :
         std::vector<std::pair<std::string, std::string>> {
             {"@task", "task/task.luau"},
             {"@schema", "schema/schema.luau"},
             {"@context/core", "context/context.luau"},
             {"@ai", "ai/ai.luau"},
             {"@http", "http/http.luau"},
             {"@json", "json/json.luau"}
         }) {
        library_entries.push_back(
            {{"name", name},
             {"source", read(libraries / file)},
             {"native", ""},
             {"dependencies", nlohmann::json::array()}}
        );
    }
    const auto library_manifest = sources / "libraries.json";
    write(
        library_manifest,
        nlohmann::ordered_json {
            {"format", "entisium.luau-libraries"},
            {"version", 1},
            {"libraries", library_entries}
        }.dump()
    );
    const std::vector manifests {manifest};
    const auto database = ets::luau_defgen::load_manifests(manifests);
    const auto summary = ets::luau_defgen::emit_definitions(
        database,
        manual,
        output,
        runtime,
        library_manifest
    );

    const auto globals = read(output / "globals.d.luau");
    CHECK(summary.class_count == 3);
    CHECK(
        globals.find(
            "load: <T>(self: __Entisium_ets_AssetServer, type: "
            "TypeToken<T>, path: AssetPath | string) -> Handle<T>"
        ) != std::string::npos
    );
    CHECK(
        globals.find("function load_error(self, key: any): AssetLoadError?") !=
        std::string::npos
    );
    CHECK(
        globals.find("function load_error(self, handle:") == std::string::npos
    );
}
