local function find_llvm_root(package, find_tool)
    local function has_libclang(root)
        return root and os.isfile(path.join(root, "include", "clang-c", "Index.h"))
    end

    local root = os.getenv("LLVM_ROOT")
    if has_libclang(root) then
        return root
    end

    local llvm = package:dep("llvm")
    root = llvm and llvm:installdir()
    if has_libclang(root) then
        return root
    end

    local clang = find_tool("clang")
    root = clang and path.directory(path.directory(clang.program))
    if has_libclang(root) then
        return root
    end
end

package("llvm-libclang")
    set_kind("library")
    add_deps("llvm >=20.1.8 <22.0.0")
    on_fetch(function(package)
        local root = find_llvm_root(package, import("lib.detect.find_tool"))
        if root then
            return {
                includedirs = path.join(root, "include"),
                linkdirs = path.join(root, "lib"),
                links = package:is_plat("windows") and "libclang" or "clang",
                version = package:dep("llvm"):version()
            }
        end
    end)
    on_load(function(package)
        local root = find_llvm_root(package, import("lib.detect.find_tool"))
        if root then
            package:addenv("PATH", path.join(root, "bin"))
        end
    end)
package_end()

if is_plat("wasm") then
    add_requires("cli11", "llvm-libclang", {host = true})
else
    add_requires("cli11", "llvm-libclang")
end

task("reflgen")
    on_run(function ()
        import("reflgen.task", {
            rootdir = path.join(os.projectdir(), "tools")
        }).run()
    end)

    set_menu {
        usage = "xmake reflgen",
        description = "Generate reflection metadata for the project.",
    }

target("entisium-reflgen")
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
    add_files("*.cpp", "../luau_defgen/type_mapper.cpp", "../luau_defgen/model.cpp")
    add_headerfiles("*.hpp")
    add_packages("llvm-libclang", "cli11", "nlohmann_json")

target("entisium-reflgen-tests")
    set_kind("binary")
    set_default(false)
    add_rules("entisium.test")
    add_files("manifest.cpp", "metadata.cpp", "model.cpp", "parser.cpp", "library_codegen.cpp", "codegen.cpp", "../luau_defgen/type_mapper.cpp", "../luau_defgen/model.cpp", "tests/*.cpp")
    add_includedirs(".")
    add_packages("nlohmann_json", "llvm-libclang")

rule("entisium.reflect.file")
    set_extensions(".reflgen")

    on_buildcmd_file(function(target, batchcmds, sourcefile, opt)
        import("reflgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).buildcmd_file(target, batchcmds, sourcefile, opt)
    end)
rule_end()

rule("entisium.reflect.module")
    set_extensions(".reflmod")
    add_orders("entisium.reflect.file", "entisium.reflect.module")

    on_buildcmd_file(function(target, batchcmds, sourcefile, opt)
        import("reflgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).buildcmd_module(target, batchcmds, opt)
    end)
rule_end()

rule("entisium.reflect.aggregate")
    set_extensions(".reflagg")
    add_orders("entisium.reflect.module", "entisium.reflect.aggregate")

    on_buildcmd_file(function(target, batchcmds, sourcefile, opt)
        import("reflgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).buildcmd_aggregate(target, batchcmds, opt)
    end)
rule_end()

rule("entisium.reflect")
    add_deps(
        "entisium.reflect.file",
        "entisium.reflect.module",
        "entisium.reflect.aggregate"
    )

    on_load(function(target)
        local reflgen = import("reflgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        })
        reflgen.configure_target(target)
    end)

    after_clean(function(target)
        import("reflgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).clean(target)
    end)

    after_build(function(target)
        import("reflgen.rules", {
            rootdir = path.join(os.projectdir(), "tools")
        }).validate_aggregate(target)
    end)
rule_end()
