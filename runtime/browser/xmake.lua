target("entisium-editor-runtime-clock")
    set_kind("static")
    add_headerfiles("src/project_clock.hpp")
    add_files("src/project_clock.cpp")
    add_includedirs("src", {public = true})
    add_deps("entisium-core")

if not is_plat("wasm") then
target("entisium-editor-runtime-clock-tests")
    set_kind("binary")
    set_default(false)
    add_rules("entisium.test")
    add_files("tests/*.cpp")
    add_deps("entisium-editor-runtime-clock")
end

if is_plat("wasm") then
local runtime_dependencies = {
        "entisium-core",
        "entisium-editor-runtime-clock",
        "entisium-project-runtime",
        "entisium-project-scripting-luau",
        "entisium-runtime-inspection",
        "entisium-runtime-inspection-playtest",
        "entisium-runtime-inspection-profiling",
        "entisium-sprite",
        "entisium-ui-rendering",
        "entisium-window-browser",
        "entisium-graphics-webgpu-browser"
    }
target("entisium-editor-runtime-sdk")
    set_kind("phony")
    set_default(false)
    add_rules("entisium.luau-sdk")
    set_values("luau.sdk.host", "entisium-editor-runtime")
    add_deps(table.unpack(runtime_dependencies))
    add_deps("entisium-luau-playtest", {links = false})

target("entisium-editor-runtime")
    set_kind("binary")
    set_policy("build.fence", true)
    add_rules("entisium.reflect", "entisium.luau-definitions")
    add_files("src/main.cpp")
    add_deps(table.unpack(runtime_dependencies))
    add_browser_shell(path.join(os.scriptdir(), "shell.html"))
    after_build(function(target)
        local runtime_output = path.join(target:targetdir(), "runtime")
        os.rm(runtime_output)
        os.mkdir(runtime_output)
        os.cp(target:targetfile(), path.join(runtime_output, "index.html"))
        for _, extension in ipairs({".js", ".wasm", ".data"}) do
            local artifact = path.join(target:targetdir(), target:name() .. extension)
            if os.isfile(artifact) then
                os.cp(artifact, runtime_output)
            end
        end
    end)

end
