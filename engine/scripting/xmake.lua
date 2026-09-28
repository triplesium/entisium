target("entisium-scripting-core")
    set_kind("static")
    add_rules("entisium.reflect")
    set_values("entisium.reflect.script_module", "scripting")
    add_files("src/*.cpp", "src/compiler/*.cpp", "src/detail/*.cpp")
    add_headerfiles("include/**.hpp")
    add_includedirs("include", {public = true})
    add_deps(
        "entisium-base",
        "entisium-refl",
        "entisium-serialization",
        "entisium-ecs",
        "entisium-app",
        "entisium-asset"
    )
    add_packages("luau", "nlohmann_json", {public = true})


if not is_plat("wasm") then
    target("entisium-luau-fixture")
        set_kind("static")
        set_default(false)
        add_rules("entisium.luau_library")
        add_headerfiles("tests/library_fixture/fixture.hpp")

    target("entisium-scripting-tests")
        set_kind("binary")
        set_default(false)
        add_rules("entisium.test")
        add_files("tests/*.cpp")
        add_includedirs("src")
        add_deps("entisium-scripting", "entisium-core", "entisium-math", "entisium-luau-fixture")
        add_packages("luau")

    target("entisium-luau-multifile")
        set_kind("static")
        set_default(false)
        add_rules("entisium.luau_library")
        add_files("tests/library_fixture/multifile/**.luau")
        add_deps("entisium-luau-fixture")

    target("entisium-luau-catalog-tests")
        set_kind("binary")
        set_default(false)
        add_rules("entisium.test")
        add_files("tests/library_fixture/catalog.test.cpp")
        add_deps("entisium-luau-multifile")

    target("entisium-scripting-query-benchmark")
        set_kind("binary")
        set_default(false)
        add_files("benchmarks/query_benchmark.cpp")
        add_deps("entisium-scripting")
        add_packages("luau")
end
