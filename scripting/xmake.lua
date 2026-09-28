includes("libraries")

target("entisium-scripting")
    set_kind("phony")
    add_deps("entisium-scripting-core", "entisium-luau-json", "entisium-luau-task",
        "entisium-luau-schema", "entisium-luau-http", "entisium-luau-ai",
        "entisium-luau-context-core", "entisium-luau-context-bridge")
