target("entisium-luau-http")
    set_kind("static")
    add_rules("entisium.luau_library")
    add_files("*.cpp")
    add_headerfiles("*.hpp")
    add_files("http.luau")
    add_deps("entisium-luau-task")
    if not is_plat("wasm") then add_deps("entisium-http") end
