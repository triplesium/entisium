local project = import("core.project.project")

local function defgen_program()
    local target = project.target("entisium-luau-defgen")
    assert(target, "entisium-luau-defgen target is unavailable")
    return target:targetfile()
end

function configure_target(target)
    target:add("deps", "entisium-luau-defgen", {links = false})
    target:set(
        "values",
        "entisium.luau-definitions.output",
        path.join(target:targetdir(), "luau-definitions")
    )
end

function generate(target)
    local reflgen = import("reflgen.rules", {
        rootdir = path.join(os.projectdir(), "tools")
    })
    local manifests = reflgen.reflection_manifests(target)
    table.sort(manifests)
    assert(#manifests > 0, "entisium.luau-definitions requires reflection manifests")

    local output = target:values("entisium.luau-definitions.output")
    local manual = path.join(
        os.projectdir(),
        "tools/luau_defgen/entisium-runtime.d.luau"
    )
    local arguments = {"--manual", manual, "--output", output, "--runtime", path.join(os.projectdir(), "runtime"), "--libraries", assert(target:values("entisium.luau-library.manifest"))}
    for _, manifest in ipairs(manifests) do
        table.insert(arguments, "--manifest")
        table.insert(arguments, manifest)
    end

    cprint("${color.build.object}generating.luau-definitions %s", target:name())
    os.vrunv(defgen_program(), arguments)
end
