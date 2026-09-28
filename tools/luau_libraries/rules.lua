local function write_changed(file, text)
    os.mkdir(path.directory(file))
    if not os.isfile(file) or io.readfile(file) ~= text then io.writefile(file, text) end
end

local function symbol(target)
    return "luau_library_" .. target:name():gsub(".", function(c) return string.format("%02x", c:byte()) end)
end

local function stable_json(value)
    import("core.base.json")
    if type(value) ~= "table" then return json.encode(value) end
    local parts = {}
    if #value > 0 then
        for _, item in ipairs(value) do table.insert(parts, stable_json(item)) end
        return "[" .. table.concat(parts, ",") .. "]"
    end
    local keys = {}
    for key in pairs(value) do table.insert(keys, key) end
    if #keys == 0 then return json.encode(value) end
    table.sort(keys)
    for _, key in ipairs(keys) do table.insert(parts, json.encode(key) .. ":" .. stable_json(value[key])) end
    return "{" .. table.concat(parts, ",") .. "}"
end

local function library_dependencies(target, transitive)
    local result, visited = {}, {}
    local function visit(owner)
        -- deps() also contains transitive targets; use declared edges to preserve boundaries.
        for _, name in ipairs(table.wrap(owner:get("deps"))) do
            local dep = assert(owner:dep(name))
            -- SDK-only dependencies describe editor APIs, not executable imports.
            if not visited[dep:name()] and not dep:rule("entisium.luau-sdk") then
                visited[dep:name()] = true
                if dep:rule("entisium.luau_library") then table.insert(result, dep) end
                if transitive then visit(dep) end
            end
        end
    end
    visit(target)
    table.sort(result, function(a, b) return a:name() < b:name() end)
    return result
end

local function load_manifest(target)
    import("core.base.json")
    return json.loadfile(assert(target:values("entisium.luau-library.manifest")))
end

function configure_library(target)
    target:set("policy", "build.fence", true)
    target:add("deps", "entisium-reflgen", {links = false})
    target:add("deps", "entisium-scripting-core")
    local output = path.join(target:autogendir(), "luau-libraries")
    target:set("values", "entisium.luau-library.manifest", path.join(output, "libraries.json"))
    target:set("values", "entisium.luau-library.function", symbol(target))
    target:add("files", path.join(output, "libraries.cpp"), {always_added = true})
end

function generate_library(target)
    import("core.base.json")
    import("core.project.depend")
    import("core.project.project")
    local output = path.join(target:autogendir(), "luau-libraries")
    local headers, sources, definitions = {}, {}, {}
    for _, file in ipairs(target:headerfiles()) do table.insert(headers, path.absolute(file)) end
    for _, file in ipairs(target:sourcefiles()) do
        if file:endswith(".d.luau") then table.insert(definitions, path.absolute(file))
        elseif file:endswith(".luau") then table.insert(sources, path.absolute(file)) end
    end
    table.sort(headers)
    table.sort(sources)
    assert(#definitions <= 1, target:name() .. ": expected at most one .d.luau native contract")
    local entry
    if #sources == 1 then entry = sources[1]
    elseif #sources > 1 then
        for _, file in ipairs(sources) do
            if path.filename(file) == "init.luau" then
                assert(not entry, target:name() .. ": ambiguous init.luau entry")
                entry = file
            end
        end
        assert(entry, target:name() .. ": multiple Luau sources require init.luau")
    end
    local deps, files, fields = {}, {}, {}
    for _, dep in ipairs(library_dependencies(target, false)) do
        for _, library in ipairs(load_manifest(dep).libraries) do table.insert(deps, library.name) end
        table.insert(files, dep:values("entisium.luau-library.manifest"))
    end
    table.sort(deps)
    local function field(key, value) table.insert(fields, json.encode(key) .. ":" .. json.encode(value)) end
    if #headers > 0 then field("headers", headers) end
    field("fallback_name", target:values("luau.name") or ("@" .. (target:name():match("^entisium%-luau%-(.+)$") or target:name())))
    if entry then field("source", entry) end
    if #definitions == 1 then field("definitions", definitions[1]) end
    if #deps > 0 then field("dependencies", deps) end
    local modules = {}
    for _, file in ipairs(sources) do
        if file ~= entry then
            local relative = path.relative(file, path.directory(entry)):gsub("\\", "/")
            assert(not relative:startswith("../"), "Library sources must be inside the entry directory")
            table.insert(modules, '{"path":' .. json.encode(relative) .. ',"file":' .. json.encode(file) .. '}')
        end
    end
    if #modules > 0 then table.insert(fields, '"modules":[' .. table.concat(modules, ",") .. ']') end
    local descriptor = path.absolute(path.join(output, "library.json"))
    local catalog = path.join(output, "inputs.json")
    write_changed(descriptor, "{" .. table.concat(fields, ",") .. "}")
    write_changed(catalog, json.encode({descriptor}))
    local program = project.target("entisium-reflgen"):targetfile()
    local arguments = {"--library-part", "--libraries", catalog, "--output", path.join(output, "libraries.cpp"),
        "--manifest-output", path.join(output, "libraries.json"), "--function", symbol(target)}
    local includes = {}
    for _, owner in ipairs(table.join({target}, target:orderdeps())) do
        for _, dir in ipairs(table.wrap(owner:get("includedirs"))) do includes[path.absolute(dir)] = true end
        for _, package in ipairs(owner:orderpkgs()) do
            for _, dir in ipairs(table.join(table.wrap(package:get("includedirs")), table.wrap(package:get("sysincludedirs")))) do includes[path.absolute(dir)] = true end
        end
    end
    local sorted = {}
    for dir in pairs(includes) do table.insert(sorted, dir) end
    table.sort(sorted)
    for _, dir in ipairs(sorted) do table.insert(arguments, "-I"); table.insert(arguments, dir) end
    table.join2(files, {catalog, descriptor, program, path.join(os.projectdir(), "tools/luau_libraries/rules.lua")})
    table.join2(files, headers, sources, definitions)
    local manifest = path.join(output, "libraries.json")
    local missing_outputs = false
    if os.isfile(manifest) then
        local previous = json.loadfile(manifest)
        for _, file in ipairs(previous.generated_files or {}) do
            if not os.isfile(file) then missing_outputs = true end
        end
        for _, file in ipairs(previous.dependencies or {}) do
            if os.isfile(file) then table.insert(files, file) end
        end
    end
    depend.on_changed(function()
        print("generating.luau-library " .. target:name())
        os.vrunv(program, arguments)
    end, {files = files, values = arguments, dependfile = path.join(output, "generate.d"),
        changed = missing_outputs or not os.isfile(path.join(output, "libraries.cpp")) or not os.isfile(manifest)})
end

function configure_catalog(target)
    local sdk = target:rule("entisium.luau-sdk")
    if not sdk and target:kind() ~= "binary" and target:kind() ~= "shared" then return end
    local scripting = false
    for _, dep in ipairs(target:orderdeps()) do
        if dep:name() == "entisium-scripting-core" then scripting = true end
    end
    if not scripting then return end
    local output = path.join(target:autogendir(), "luau-catalog")
    target:set("values", "entisium.luau-catalog.output", output)
    target:set("values", "entisium.luau-library.manifest", path.join(output, "libraries.json"))
    if not sdk then target:add("files", path.join(output, "catalog.cpp"), {always_added = true}) end
end

function generate_catalog(target)
    import("core.base.json")
    local output = target:values("entisium.luau-catalog.output")
    if not output then return end
    local declarations, calls, libraries, names = {}, {}, {}, {}
    for _, dep in ipairs(library_dependencies(target, true)) do
        local func = symbol(dep)
        table.insert(declarations, "std::span<const LuauLibraryDefinition> " .. func .. "();")
        table.insert(calls, "for (const auto& library : " .. func .. "()) result.push_back(library);")
        for _, library in ipairs(load_manifest(dep).libraries) do
            assert(not names[library.name], "Duplicate Luau library: " .. library.name)
            names[library.name] = library
            table.insert(libraries, library)
        end
    end
    local marks = {}
    local function visit(library)
        assert(marks[library.name] ~= 1, "Circular Luau dependency: " .. library.name)
        if marks[library.name] == 2 then return end
        marks[library.name] = 1
        for _, name in ipairs(library.dependencies or {}) do
            assert(names[name], "Missing Luau dependency: " .. name)
            visit(names[name])
        end
        marks[library.name] = 2
    end
    for _, library in ipairs(libraries) do visit(library) end
    if not target:rule("entisium.luau-sdk") then
        write_changed(path.join(output, "catalog.cpp"),
            '#include "scripting/library.hpp"\nnamespace ets {\n' .. table.concat(declarations, "\n") ..
        '\nstd::span<const LuauLibraryDefinition> default_luau_libraries() {\n' ..
        'static const auto libraries = [] { std::vector<LuauLibraryDefinition> result;\n' .. table.concat(calls, "\n") ..
        '\nreturn result; }(); return libraries;\n}\n}\n')
    end
    write_changed(path.join(output, "libraries.json"),
        stable_json({format = "entisium.luau-libraries", version = 1, libraries = json.mark_as_array(libraries)}))
end
