target("entisium-luau-defgen")
    set_kind("binary")
    set_default(false)
    set_policy("build.fence", true)
    if is_plat("wasm") then
        set_plat(os.host())
        set_arch(os.arch())
        if is_host("windows") then
            set_toolchains("msvc")
        elseif is_host("macosx") then
            set_toolchains("clang")
        else
            set_toolchains("gcc")
        end
    end
    add_files("*.cpp")
    add_headerfiles("*.hpp")
    add_packages("cli11", "nlohmann_json")

target("entisium-luau-defgen-tests")
    set_kind("binary")
    set_default(false)
    add_rules("entisium.test")
    add_files("emitter.cpp", "manifest.cpp", "model.cpp", "type_mapper.cpp", "tests/*.cpp")
    add_includedirs(".")
    add_packages("nlohmann_json")

rule("entisium.luau-definitions")
    on_load(function(target)
        import("luau_defgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).configure_target(target)
    end)

rule_end()

rule("entisium.luau-sdk")
    on_load(function(target)
        import("luau_defgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).configure_sdk(target)
    end)

    after_load(function(target)
        import("luau_defgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).configure_sdk_output(target)
    end)

    on_build(function(target)
        import("luau_defgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).generate(target)
    end)
rule_end()
