set_project("Entisium")
add_rules("mode.debug", "mode.release")
set_languages("c++23")
set_warnings("all")

option("tracy")
    set_default(false)
    set_showmenu(true)
    set_description("Enable Tracy CPU profiling")
option_end()

option("profile_summary")
    set_default(false)
    set_showmenu(true)
    set_description("Enable engine-side profiling summary output")
option_end()

option("shader_slang_sdk")
    set_default(os.getenv("VULKAN_SDK") or "")
    set_showmenu(true)
    set_description("Path to a Slang SDK used by runtime shader compilation")
option_end()

option("shader_spirv_cross_sdk")
    set_default(os.getenv("VULKAN_SDK") or "")
    set_showmenu(true)
    set_description("Path to a Vulkan/SPIRV-Cross SDK used by shader artifact generation")
option_end()

option("shader_targets")
    set_default("opengl,vulkan,webgpu")
    set_showmenu(true)
    set_description("Comma-separated runtime shader targets to build")
option_end()

option("editor_demo_project")
    set_default("samples/projects/skyline_strike")
    set_showmenu(true)
    set_description("Project directory bundled by entisium-editor-demo")
option_end()

option("tests")
    set_default(false)
    set_showmenu(true)
    set_description("Build and register test targets")
option_end()

if is_plat("windows") and has_config("profile_summary") then
    set_symbols("debug")
    set_strip("none")
    add_ldflags("/DEBUG:FULL", "/INCREMENTAL:NO", {force = true})
end

includes("packages")

if has_config("tests") then
    add_requires("catch2 v3.15.2")
end

if is_plat("wasm") then
    add_requires("emscripten 6.0.0")
    add_requires(
        "entisium-slang-wasm 2026.14.1",
        {configs = {toolchains = "entisium-emcc@emscripten"}}
    )
    set_toolchains("entisium-emcc@emscripten")
end

if is_plat("wasm") then
    add_requires(
        "stb",
        "tinyobjloader",
        "mikktspace",
        "nlohmann_json",
        "yaml-cpp"
    )
    add_requires(
        "luau 0.735",
        {
            configs = {
                shared = false,
                extern_c = false,
                cxflags = "-fwasm-exceptions",
                cxxflags = "-fwasm-exceptions",
            }
        }
    )
    add_requires(
        "luau-lsp 2026.8.25-entisium.5",
        {
            configs = {
                shared = false,
                toolchains = "entisium-emcc@emscripten",
                cxflags = "-pthread -fwasm-exceptions",
                cxxflags = "-pthread -fwasm-exceptions",
            }
        }
    )
else
    add_requires("stb", "glad", "tinyobjloader", "mikktspace", "nlohmann_json", "fastgltf v0.9.0")
    add_requires("cpp-httplib v0.53.1", {configs = {ssl = true}})
    add_requires("libcurl 8.21.0", {configs = {shared = false, cares = true}})
    add_requires("box2d v3.1.1", {configs = {shared = false}})
    add_requires("luau 0.735", {configs = {shared = false, extern_c = false}})
    add_requires("luau-lsp 2026.8.25-entisium.5", {configs = {shared = false}})
    add_requires("yaml-cpp")
    add_requires("glfw", {configs = {shared = false}})
    add_requires("imgui v1.92.7-docking", {configs = {glfw = true, opengl3 = false}})
end
if has_config("tracy") then
    add_requires(
        "tracy v0.13.0",
        {
            configs = {
                on_demand = true,
                enforce_callstack = false,
                callstack = false,
                code_transfer = false,
                context_switch = false,
                broadcast = false,
                sampling = false,
                verify = false,
                vsync_capture = false,
                system_tracing = false,
                frame_image = false,
                fibers = false,
                crash_handler = false,
            }
        }
    )
    if is_plat("windows") then
        add_ldflags("/INCREMENTAL:NO", {force = true})
    end
end

set_policy("check.auto_ignore_flags", false)
if is_plat("windows") then
    set_policy("run.windows_error_dialog", false)
end

local project_dir = os.scriptdir():gsub("\\", "/")
if is_plat("wasm") then
    add_defines("ETS_ASSETS_PATH=\"/entisium/assets\"")
else
    add_defines("ETS_ASSETS_PATH=\"" .. project_dir .. "/assets\"")
end
add_defines("ETS_SHADER_ASSETS_PATH=\"" .. project_dir .. "/build/generated/shaders\"")
add_defines("ETS_SHADER_CACHE_PATH=\"" .. project_dir .. "/build/cache/shaders\"")
add_defines("ETS_PROFILE_OUTPUT_PATH=\"" .. project_dir .. "/build/profile/latest\"")

local shader_sources = {}

function add_embedded_asset(name, file)
    local normalized_name = name:gsub("\\", "/")
    if #normalized_name == 0 or normalized_name:sub(1, 1) == "/" or
        normalized_name:find("..", 1, true) then
        raise("invalid embedded asset name: " .. name)
    end

    add_values(
        "entisium.embedded_assets",
        normalized_name .. "=" .. path.absolute(file):gsub("\\", "/"),
        {public = true}
    )
end

function add_asset_bundle(prefix, root)
    if not prefix or #prefix == 0 then
        raise("asset bundle prefix is required")
    end
    if not root or #root == 0 then
        raise("asset bundle root is required")
    end

    add_values(
        "entisium.asset_bundles",
        prefix:gsub("\\", "/") .. "=" ..
            path.absolute(root):gsub("\\", "/")
    )
end

function add_browser_shell(shell_file)
    add_ldflags(
        "--shell-file",
        shell_file,
        "-sEXPORTED_RUNTIME_METHODS=ccall",
        {force = true}
    )
    add_extrafiles(shell_file)
    if has_config("profile_summary") then
        add_ldflags("--emit-symbol-map", {force = true})
    end

    before_build(function(target)
        local html_path = target:targetfile()
        if os.isfile(html_path) and os.mtime(shell_file) > os.mtime(html_path) then
            os.rm(html_path)
        end
    end)

    after_link(function(target)
        local html_path = target:targetfile()
        local html = io.readfile(html_path)
        local script_name = target:name() .. ".js"
        local profile_build_id = ""
        if has_config("profile_summary") then
            import("core.base.json")

            local wasm_path = path.join(
                target:targetdir(),
                target:name() .. ".wasm"
            )
            local digest = hash.sha256(wasm_path)
            profile_build_id = "wasm:" .. digest

            local symbol_map_path = html_path .. ".symbols"
            assert(
                os.isfile(symbol_map_path),
                "missing Emscripten symbol map: " .. symbol_map_path
            )
            local symbols = {}
            for line in io.readfile(symbol_map_path):gmatch("[^\r\n]+") do
                local symbol_id, function_name = line:match("^(%d+):(.*)$")
                if symbol_id and function_name then
                    symbols[symbol_id] = { ["function"] = function_name }
                end
            end
            local symbol_directory = path.join(
                target:targetdir(),
                "profile-symbols"
            )
            os.mkdir(symbol_directory)
            io.writefile(
                path.join(symbol_directory, digest .. ".json"),
                json.encode({
                    schema = "entisium.profile-symbols.v1",
                    module_id = profile_build_id,
                    kind = "wasm-function-index",
                    symbols = symbols,
                })
            )
        end
        if html:find(script_name .. "?dev=", 1, true) then
            return
        end
        local script_pattern = script_name:gsub("(%W)", "%%%1")
        local script_tag = '<script[^>]-src=["\']?' ..
            script_pattern .. '["\']?[^>]*></script>'
        local script_loader = [[<script>
            globalThis.ETS_PROFILE_BUILD_ID = "]] ..
            profile_build_id .. [[";
            const emscriptenScript = document.createElement("script");
            emscriptenScript.async = true;
            emscriptenScript.src = "]] .. script_name .. [[?dev=" + resourceVersion;
            document.body.appendChild(emscriptenScript);
        </script>]]
        local replacement_count
        html, replacement_count = html:gsub(script_tag, script_loader, 1)
        assert(
            replacement_count == 1,
            "failed to add cache busting to " .. script_name
        )
        io.writefile(html_path, html)
    end)
end

local function target_asset_bundles(target)
    local bundles = {}
    for _, entry in ipairs(table.wrap(target:values("entisium.asset_bundles"))) do
        local separator = entry:find("=", 1, true)
        if not separator then
            raise("invalid asset bundle: " .. entry)
        end
        table.insert(bundles, {
            prefix = entry:sub(1, separator - 1),
            root = entry:sub(separator + 1),
        })
    end
    return bundles
end

local function target_embedded_assets(target)
    local assets = {}
    local owners = {target}
    table.join2(owners, target:orderdeps())
    for _, owner in ipairs(owners) do
        for _, entry in ipairs(
            table.wrap(owner:values("entisium.embedded_assets"))
        ) do
            local separator = entry:find("=", 1, true)
            if not separator then
                raise("invalid embedded asset: " .. entry)
            end
            local name = entry:sub(1, separator - 1)
            local file = entry:sub(separator + 1)
            local existing = assets[name]
            if existing and existing ~= file then
                raise("embedded asset name already registered: " .. name)
            end
            assets[name] = file
        end
    end
    return assets
end

function add_shader_source(prefix, root)
    if not prefix or #prefix == 0 then
        raise("shader source prefix is required")
    end
    if not root or #root == 0 then
        raise("shader source root is required")
    end

    local normalized_prefix = prefix:gsub("\\", "/")
    local normalized_root = path.absolute(root):gsub("\\", "/")
    table.insert(shader_sources, {
        prefix = normalized_prefix,
        root = normalized_root,
    })
end

local function shader_runtime_root(source, target)
    if target:is_plat("wasm") then
        return "/entisium/shaders/" .. source.prefix
    end
    return source.root
end

local function shader_sources_define_value(target)
    table.sort(shader_sources, function(a, b)
        return a.prefix < b.prefix
    end)

    local entries = {}
    for _, source in ipairs(shader_sources) do
        table.insert(
            entries,
            source.prefix .. "=" .. shader_runtime_root(source, target)
        )
    end
    return table.concat(entries, ";")
end

rule("entisium.shader_sources")
    after_load(function(target)
        if target:values("entisium.skip_runtime_assets") then
            return
        end
        local sources = shader_sources_define_value(target)
        if sources and #sources > 0 then
            target:add("defines", "ETS_SHADER_SOURCES=\"" .. sources .. "\"")
        end
        if target:is_plat("wasm") and target:kind() == "binary" then
            for _, source in ipairs(shader_sources) do
                target:add(
                    "ldflags",
                    "--preload-file=" .. source.root .. "@" ..
                        shader_runtime_root(source, target),
                    {force = true}
                )
                target:add("extrafiles", path.join(source.root, "**.slang"))
            end
        end
    end)
    before_build(function(target)
        if target:values("entisium.skip_runtime_assets") then
            return
        end
        if not target:is_plat("wasm") or target:kind() ~= "binary" then
            return
        end

        import("core.project.depend")
        local source_files = {}
        for _, source in ipairs(shader_sources) do
            table.join2(
                source_files,
                os.files(path.join(source.root, "**.slang"))
            )
        end
        table.sort(source_files)

        depend.on_changed(function()
            os.rm(target:targetfile())
        end, {
            dependfile = target:dependfile(
                path.join(target:autogendir(), "shader_sources")
            ),
            files = source_files,
            values = source_files,
        })
    end)
rule_end()

add_rules("entisium.shader_sources")

rule("entisium.embedded_assets")
    after_load(function(target)
        if target:values("entisium.skip_runtime_assets") then
            return
        end
        if not target:is_plat("wasm") or target:kind() ~= "binary" then
            return
        end

        local assets = target_embedded_assets(target)
        local names = table.keys(assets)
        table.sort(names)
        for _, name in ipairs(names) do
            local file = assets[name]
            target:add(
                "ldflags",
                "--preload-file=" .. file .. "@/entisium/embedded/" .. name,
                {force = true}
            )
            target:add("extrafiles", file)
        end
    end)
    before_build(function(target)
        if target:values("entisium.skip_runtime_assets") then
            return
        end
        if not target:is_plat("wasm") or target:kind() ~= "binary" then
            return
        end

        import("core.project.depend")
        local files = table.values(target_embedded_assets(target))
        table.sort(files)
        depend.on_changed(function()
            os.rm(target:targetfile())
        end, {
            dependfile = target:dependfile(
                path.join(target:autogendir(), "embedded_assets")
            ),
            files = files,
            values = files,
        })
    end)
rule_end()

add_rules("entisium.embedded_assets")

rule("entisium.asset_bundles")
    after_load(function(target)
        if target:values("entisium.skip_runtime_assets") then
            return
        end
        if not target:is_plat("wasm") or target:kind() ~= "binary" then
            return
        end

        for _, bundle in ipairs(target_asset_bundles(target)) do
            target:add(
                "ldflags",
                "--preload-file=" .. bundle.root .. "@/entisium/assets/" ..
                    bundle.prefix,
                {force = true}
            )
            target:add("extrafiles", path.join(bundle.root, "**"))
        end
    end)
    before_build(function(target)
        if target:values("entisium.skip_runtime_assets") then
            return
        end
        if not target:is_plat("wasm") or target:kind() ~= "binary" then
            return
        end

        import("core.project.depend")
        local bundles = target_asset_bundles(target)
        if #bundles == 0 then
            return
        end

        local asset_files = {}
        local bundle_values = {}
        for _, bundle in ipairs(bundles) do
            table.join2(asset_files, os.files(path.join(bundle.root, "**")))
            table.insert(
                bundle_values,
                bundle.prefix .. "=" .. bundle.root
            )
        end
        table.sort(asset_files)
        table.sort(bundle_values)

        depend.on_changed(function()
            os.rm(target:targetfile())
        end, {
            dependfile = target:dependfile(
                path.join(target:autogendir(), "asset_bundles")
            ),
            files = asset_files,
            values = bundle_values,
        })
    end)
rule_end()

add_rules("entisium.asset_bundles")

rule("entisium.executable_startup")
    on_load(function(target)
        if target:kind() == "binary" then
            target:add(
                "files",
                path.join(
                    os.projectdir(),
                    "engine/base/src/startup/crt_report.cpp"
                )
            )
        end
    end)
rule_end()

add_rules("entisium.executable_startup")

rule("entisium.test")
    add_deps("entisium.reflect")

    on_load(function(target)
        if not has_config("tests") then
            target:set("enabled", false)
            return
        end
        target:add("deps", "entisium-refl")
        target:add("packages", "catch2")
        target:add("tests", "default")
    end)
rule_end()

add_cxxflags("cl::/Zc:preprocessor")

if is_plat("wasm") then
    includes("tools/reflgen")
    includes("tools/luau_defgen")
    includes("engine")
    includes("samples/browser")
    includes("samples/browser_project")
else
    includes("tools")
    includes("engine")
    includes("samples")
    includes("tests")
end

includes("runtime")
includes("editor")

if is_plat("wasm") then
    includes("tools/entisium_lsp")
end
