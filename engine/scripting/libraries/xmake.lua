includes("*/xmake.lua")

target("entisium-scripting")
    set_kind("phony")
    add_deps("entisium-scripting-core", "entisium-luau-json", "entisium-luau-task",
        "entisium-luau-schema", "entisium-luau-http", "entisium-luau-ai",
        "entisium-luau-context-core", "entisium-luau-context-bridge")

if not is_plat("wasm") then
target("entisium-luau-libraries-tests")
    set_kind("binary")
    set_default(false)
    add_rules("entisium.test")
    add_files("*/tests/*.cpp")
    add_deps("entisium-scripting")
    add_defines("ETS_PROJECT_ROOT=\"" .. os.projectdir():gsub("\\", "/") .. "\"")

    add_packages("cpp-httplib")
end
