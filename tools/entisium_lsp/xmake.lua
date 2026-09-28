if not is_plat("wasm") then
target("entisium-lsp")
    set_kind("binary")
    set_default(false)
    add_files("src/*.cpp")
    remove_files("src/wasm_*.cpp")
    add_includedirs("../../engine/scripting/include")
    add_packages("luau-lsp")

if has_config("tests") then
    target("entisium-lsp-tests")
        set_kind("binary")
        set_default(false)
        add_rules("entisium.test")
        add_files(
            "tests/*.cpp",
            "src/completion_detail.cpp",
            "src/definition_index.cpp",
            "src/diagnostics.cpp",
            "src/hover_signature.cpp",
            "src/internal_type_hover.cpp",
            "src/public_type_names.cpp",
            "src/script_type_registry.cpp",
            "src/script_type_values.cpp",
            "src/wasm_transport.cpp"
        )
        add_includedirs("src", "../../engine/scripting/include")
        add_packages("luau-lsp")
        add_deps("entisium-scripting")
        after_load(function(target)
            local catalog = path.absolute(path.join(target:autogendir(), "luau-catalog", "libraries.json")):gsub("\\", "/")
            target:add("defines", 'ETS_TEST_LUAU_CATALOG="' .. catalog .. '"')
        end)
        add_defines(
            "ETS_PROJECT_ROOT=\"" .. os.projectdir():gsub("\\", "/") .. "\""
        )
end
end

if is_plat("wasm") then
target("entisium-lsp-wasm")
    set_kind("binary")
    set_default(false)
    set_filename("entisium-lsp.mjs")
    set_values("entisium.skip_runtime_assets", true)
    add_files("src/*.cpp")
    remove_files("src/main.cpp")
    add_includedirs("src", "../../engine/scripting/include")
    add_packages("luau-lsp")
    add_deps("entisium-editor-runtime", {links = false})
    add_cxflags(
        "-pthread",
        "-fwasm-exceptions",
        {force = true}
    )
    add_ldflags(
        "-pthread",
        "-fwasm-exceptions",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sDEFAULT_PTHREAD_STACK_SIZE=4194304",
        "-sENVIRONMENT=worker",
        "-sEXPORT_ES6=1",
        "-sEXPORT_NAME=createEntisiumLsp",
        "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_ets_lsp_start','_ets_lsp_send','_ets_lsp_take_output','_ets_lsp_free','_ets_lsp_last_error']",
        "-sEXPORTED_RUNTIME_METHODS=['ccall','UTF8ToString','FS']",
        "-sFORCE_FILESYSTEM=1",
        "-sMODULARIZE=1",
        "-sNO_EXIT_RUNTIME=1",
        "-sPTHREAD_POOL_SIZE=2",
        "-sPTHREAD_POOL_SIZE_STRICT=1",
        {force = true}
    )
    after_load(function(target)
        local definitions = path.join(
            assert(target:dep("entisium-editor-runtime")):targetdir(),
            "luau-definitions"
        )
        target:add(
            "ldflags",
            "--preload-file=" .. definitions ..
                "@/entisium/luau-definitions",
            {force = true}
        )
    end)
end
