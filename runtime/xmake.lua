includes("browser")

if not is_plat("wasm") then
    includes("native")
    includes("playtest")
end
