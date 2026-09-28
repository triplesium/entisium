local host_dependencies = {"entisium-scripting", "entisium-runtime-host-core", "entisium-luau-playtest"}

target("entisium-luau-host")
    set_kind("binary")
    add_rules("entisium.reflect", "entisium.luau-definitions")
    add_files("src/*.cpp")
    add_deps(table.unpack(host_dependencies))
    add_packages("nlohmann_json")

target("entisium-luau-host-sdk")
    set_kind("phony")
    set_default(false)
    add_rules("entisium.luau-sdk")
    set_values("luau.sdk.host", "entisium-luau-host")
    add_deps(table.unpack(host_dependencies))
