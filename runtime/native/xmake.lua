local host_dependencies = {"entisium-runtime-host-core", "entisium-project"}

target("entisium-runtime-host-core")
    set_kind("static")
    add_headerfiles("include/**.hpp")
    add_files(
        "src/application.cpp",
        "src/test_snapshot.cpp",
        "src/quick_save.cpp",
        "src/snapshot_archive.cpp"
    )
    add_includedirs("include", {public = true})
    add_deps(
        "entisium-base",
        "entisium-app",
        "entisium-core",
        "entisium-project",
        "entisium-project-runtime",
        "entisium-project-scripting-luau",
        "entisium-window",
        "entisium-window-glfw",
        "entisium-input",
        "entisium-rendering",
        "entisium-sprite",
        "entisium-input-focus",
        "entisium-physics2d",
        "entisium-text",
        "entisium-ui",
        "entisium-ui-widgets",
        "entisium-ui-rendering",
        "entisium-graphics-opengl",
        "entisium-graphics-opengl-glfw",
        "entisium-runtime-protocol",
        "entisium-runtime-inspection-ecs",
        "entisium-runtime-inspection-snapshot",
        "entisium-snapshot-runtime",
        "entisium-snapshot-runtime-asset",
        "entisium-snapshot-runtime-luau",
        "entisium-snapshot-runtime-physics2d",
        "entisium-snapshot-runtime-rendering",
        "entisium-snapshot-runtime-ui"
    )
    add_packages("glfw", "nlohmann_json", "stb")

target("entisium-runtime-host")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_rules("entisium.reflect", "entisium.luau-definitions")
    add_files("src/main.cpp")
    add_deps(table.unpack(host_dependencies))
    add_packages("glfw", "glad")

target("entisium-runtime-host-sdk")
    set_kind("phony")
    set_default(false)
    add_rules("entisium.luau-sdk")
    set_values("luau.sdk.host", "entisium-runtime-host")
    add_deps(table.unpack(host_dependencies))
    add_deps("entisium-luau-playtest", {links = false})

target("entisium-runtime-host-tests")
    set_kind("binary")
    set_default(false)
    add_rules("entisium.test")
    add_files("tests/*.cpp")
    add_deps("entisium-runtime-host-core")
    add_packages("nlohmann_json")
