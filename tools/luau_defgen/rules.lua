local project = import("core.project.project")

local function sdk_output(host)
    return path.join(host:targetdir(), "luau-definitions", host:name())
end

function configure_target(target)
    target:add("deps", target:name() .. "-sdk", {links = false})
    target:set("values", "entisium.luau-definitions.output", sdk_output(target))
end

function configure_sdk(target)
    target:set("policy", "build.fence", true)
    target:add("deps", "entisium-luau-defgen", {links = false})
end

function configure_sdk_output(target)
    local host = assert(project.target(target:values("luau.sdk.host")), "Luau SDK host is unavailable")
    assert(host:rule("entisium.luau-definitions"), "Luau SDK host must use entisium.luau-definitions")
    target:set("values", "entisium.luau-definitions.output", sdk_output(host))
    for _, name in ipairs(table.wrap(host:get("deps"))) do
        if name ~= target:name() then
            assert(target:dep(name), "Luau SDK must share host dependency: " .. name)
        end
    end
end

function generate(target)
    local depend = import("core.project.depend")
    local json = import("core.base.json")
    local reflgen = import("reflgen.rules", {rootdir = path.join(os.projectdir(), "tools")})
    local host = assert(project.target(target:values("luau.sdk.host")))
    -- Host-owned reflected headers also participate without compiling/linking it.
    reflgen.generate(host)
    local manifests = reflgen.reflection_manifests(host)
    table.sort(manifests)
    assert(#manifests > 0, "entisium.luau-sdk requires reflection manifests")

    local output = target:values("entisium.luau-definitions.output")
    local manual = path.join(os.projectdir(), "tools/luau_defgen/entisium-runtime.d.luau")
    local catalog = assert(target:values("entisium.luau-library.manifest"))
    local program = assert(project.target("entisium-luau-defgen")):targetfile()
    local arguments = {"--manual", manual, "--output", output, "--libraries", catalog}
    for _, manifest in ipairs(manifests) do
        table.insert(arguments, "--manifest")
        table.insert(arguments, manifest)
    end

    local inventory = path.join(output, "sdk-files.json")
    local missing = not os.isfile(inventory)
    if not missing then
        for _, file in ipairs(json.loadfile(inventory)) do
            if not os.isfile(path.join(output, file)) then missing = true; break end
        end
    end
    depend.on_changed(function()
        cprint("${color.build.object}generating.luau-definitions %s", host:name())
        os.vrunv(program, arguments)
    end, {
        files = table.join(manifests, {manual, catalog, program,
            path.join(os.projectdir(), "tools/luau_defgen/rules.lua")}),
        values = arguments,
        dependfile = target:dependfile("luau-sdk"),
        changed = missing,
    })
end
