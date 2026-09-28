#include "library_codegen.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
struct Workspace {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("ets-library-gen-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()
         ));
    Workspace() { std::filesystem::create_directories(root); }
    ~Workspace() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void write(const char* name, std::string_view text) const {
        std::ofstream(root / name) << text;
    }
    std::string read(const char* name) const {
        std::ifstream in(root / name);
        return {std::istreambuf_iterator<char>(in), {}};
    }
    void header(std::string_view methods) const {
        write(
            "api.hpp",
            std::string(
                "#define ETS_REFLECT(...)\nETS_REFLECT(LuauLibrary(name = "
                "\"@sample\"))\nstruct Api {\n"
            ) + std::string(methods) +
                "\n};\n"
        );
        write("library.json", R"({"header":"api.hpp"})");
        write(
            "inputs.json",
            nlohmann::json::array({(root / "library.json").generic_string()})
                .dump()
        );
    }
    void generate() const {
        ets::reflgen::generate_luau_libraries(
            root / "inputs.json",
            root / "out.cpp",
            root / "out.json",
            {}
        );
    }
};
} // namespace

TEST_CASE(
    "Library generation shares names and signatures with the SDK",
    "[reflgen][library]"
) {
    Workspace workspace;
    workspace.header(
        "ETS_REFLECT(LuauExport(name = \"sum\"))\nstatic int add(int a, int "
        "b);\nbool enabled() const;"
    );
    REQUIRE_NOTHROW(workspace.generate());
    const auto cpp = workspace.read("out.cpp");
    const auto manifest = nlohmann::json::parse(workspace.read("out.json"));
    CHECK(manifest["libraries"][0]["name"] == "@sample");
    const auto types = manifest["libraries"][0]["native"].get<std::string>();
    CHECK(
        types.find("sum: (a: number, b: number) -> number") != std::string::npos
    );
    CHECK(types.find("enabled: () -> boolean") != std::string::npos);
    CHECK(cpp.find("&Api::add") != std::string::npos);
    CHECK(cpp.find("&Api::enabled") != std::string::npos);
    const auto timestamp =
        std::filesystem::last_write_time(workspace.root / "out.cpp");
    workspace.generate();
    CHECK(
        std::filesystem::last_write_time(workspace.root / "out.cpp") ==
        timestamp
    );
}

TEST_CASE(
    "Library generation rejects unsupported contracts instead of skipping",
    "[reflgen][library]"
) {
    Workspace workspace;
    for (const auto* methods : {
             "static int* pointer();",
             ("ETS_REFLECT(LuauExport(signature = \"() -> number\")) static "
              "int* pointer();"),
             "static int overload(int); static int overload(double);",
             "static int broken(Unknown value);",
             "template<class T> static T generic(T value);",
             "static int variadic(int, ...);",
             "int qualified() &&;",
             "ETS_REFLECT(LuauExport(nmae = typo)) static int renamed();",
             "ETS_REFLECT(LuauExport(table = true)) static int snapshot();",
             "ETS_REFLECT(LuauExport(readonly = true)) static int readonly();",
         }) {
        INFO(methods);
        workspace.header(methods);
        CHECK_THROWS(workspace.generate());
    }
}

TEST_CASE(
    "Library targets discover marked headers and preserve private sources",
    "[reflgen][library]"
) {
    Workspace workspace;
    workspace.header("static int value();");
    workspace.write("helper.hpp", "struct Unrelated { int value; };\n");
    workspace.write("helper.luau", "return {value = 42}");
    workspace.write(
        "library.json",
        R"({"headers":["helper.hpp","api.hpp"],"fallback_name":"@ignored","dependencies":["@external"],"modules":[{"path":"helper.luau","file":"helper.luau"}]})"
    );
    CHECK_THROWS(workspace.generate());
    CHECK_NOTHROW(
        ets::reflgen::generate_luau_libraries(
            workspace.root / "inputs.json",
            workspace.root / "out.cpp",
            workspace.root / "out.json",
            {},
            "part",
            true
        )
    );
    const auto manifest = nlohmann::json::parse(workspace.read("out.json"));
    CHECK(manifest["libraries"][0]["name"] == "@sample");
    CHECK(
        manifest["libraries"][0]["modules"]["helper.luau"] ==
        "return {value = 42}"
    );
    CHECK(workspace.read("out.cpp").find(".modules=") != std::string::npos);
    workspace.write(
        "library.json",
        R"({"name":"@test","source":"helper.luau","modules":[{"path":"../escape.luau","file":"helper.luau"}]})"
    );
    CHECK_THROWS(workspace.generate());
}

TEST_CASE(
    "Library generation rejects catalog collisions missing edges and cycles",
    "[reflgen][library]"
) {
    Workspace workspace;
    workspace.write("module.luau", "return {}");
    const auto catalog = nlohmann::json::array(
        {(workspace.root / "a.json").generic_string(),
         (workspace.root / "b.json").generic_string()}
    );
    workspace.write("inputs.json", catalog.dump());
    workspace.write(
        "a.json",
        R"({"name":"@a","source":"module.luau","dependencies":["@b"]})"
    );
    workspace.write(
        "b.json",
        R"({"name":"@b","source":"module.luau","dependencies":["@a"]})"
    );
    CHECK_THROWS(workspace.generate());
    workspace.write("b.json", R"({"name":"@a","source":"module.luau"})");
    CHECK_THROWS(workspace.generate());
    workspace.write("b.json", R"({"name":"@c","source":"module.luau"})");
    CHECK_THROWS(workspace.generate());
    workspace.write("b.json", R"({"name":"@b","source":"module.luau"})");
    CHECK_NOTHROW(workspace.generate());
}

TEST_CASE(
    "Library contracts combine aliases inputs and local signatures",
    "[reflgen][library]"
) {
    Workspace workspace;
    workspace.header("static int placeholder();");
    workspace.write("api.hpp", R"cpp(
#define ETS_REFLECT(...)
ETS_REFLECT(LuauType(name = "Request", input = "{url: string} | {path: string}"))
struct Options { int count; };
ETS_REFLECT(LuauType(name = "Connection"))
class Handle { int identity; };
ETS_REFLECT(LuauLibrary(name = "@sample"))
struct Api {
    static Options request(Options value);
    static Handle connection();
    ETS_REFLECT(LuauExport(signature = "<T>(T, " "string) -> T"))
    static int copy(int value, int text);
};
)cpp");
    REQUIRE_NOTHROW(workspace.generate());
    const auto manifest = nlohmann::json::parse(workspace.read("out.json"));
    const auto types = manifest["libraries"][0]["native"].get<std::string>();
    CHECK(types.find("export type Request =") != std::string::npos);
    CHECK(
        types.find(
            "export type RequestInput = {url: string} | {path: string}"
        ) != std::string::npos
    );
    CHECK(
        types.find("request: (value: Request | RequestInput) -> Request") !=
        std::string::npos
    );
    CHECK(types.find("copy: <T>(T, string) -> T") != std::string::npos);
    CHECK(types.find("ConnectionInput") == std::string::npos);
    CHECK(types.find("__Entisium_Handle: never") != std::string::npos);
    CHECK(workspace.read("out.cpp").find("&Api::copy") != std::string::npos);
}

TEST_CASE(
    "Custom library declarations come from header metadata",
    "[reflgen][library]"
) {
    Workspace workspace;
    workspace.header("static int placeholder();");
    workspace.write("api.hpp", R"cpp(
#define ETS_REFLECT(...)
ETS_REFLECT(LuauLibrary(name = "@sample", custom = true,
    globals = "declare extern type NativeValue with\nend",
    types = "export type Value = {read value: string}",
    exports = "read item: Value, read copy: <T>(T) -> T"))
struct Api {};
)cpp");
    REQUIRE_NOTHROW(workspace.generate());
    const auto manifest = nlohmann::json::parse(workspace.read("out.json"));
    const auto types = manifest["libraries"][0]["native"].get<std::string>();
    CHECK(
        types.find("export type Value = {read value: string}") !=
        std::string::npos
    );
    CHECK(types.find("read copy: <T>(T) -> T") != std::string::npos);
    CHECK(
        manifest["libraries"][0]["globals"] ==
        "declare extern type NativeValue with\nend"
    );
    CHECK(types.find("declare extern") == std::string::npos);
}

TEST_CASE(
    "Library contracts reject enum parameters results and fields",
    "[reflgen][library]"
) {
    Workspace workspace;
    workspace.header("static int placeholder();");
    for (const auto* declaration : {
             "struct Api { static Mode mode(); };",
             "struct Api { static void set(Mode value); };",
             "struct Api { ETS_REFLECT(LuauExport(signature = \"() -> "
             "number\")) static Mode mode(); };",
             "struct Api { static Payload value(); };",
         }) {
        INFO(declaration);
        const bool payload = std::string_view(declaration).find("Payload") !=
                             std::string_view::npos;
        workspace.write(
            "api.hpp",
            std::string(
                "#define ETS_REFLECT(...)\nETS_REFLECT() enum class Mode { A, "
                "B };\n"
            ) +
                (payload ? "ETS_REFLECT() struct Payload { Mode mode; };\n" :
                           "") +
                "ETS_REFLECT(LuauLibrary(name = \"@sample\"))\n" + declaration
        );
        CHECK_THROWS_WITH(
            workspace.generate(),
            "Enum types are not supported in Luau library contracts: Mode"
        );
    }
    workspace.write("api.hpp", R"cpp(
#define ETS_REFLECT(...)
ETS_REFLECT() enum class Mode { A, B };
ETS_REFLECT(LuauLibrary(name = "@sample")) struct Api { static int value(); };
)cpp");
    CHECK_NOTHROW(workspace.generate());
}
