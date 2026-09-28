includes("*/xmake.lua")

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
