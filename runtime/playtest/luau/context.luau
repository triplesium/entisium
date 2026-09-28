--!strict
local bridge = require("@context/bridge")
local core = require("@context/core")
local scheduler = require("@playtest/scheduler")

export type ContextSource<T> = core.ContextSource<T>
export type ContextResource<T> = core.ContextResource<T>
export type ContextQuery<T> = core.ContextQuery<T>
export type ContextSample<T> = core.ContextSample<T>
export type ContextView<T> = core.ContextView<T>
export type ContextAPI = core.ContextAPI

return core.bind {
    copy_value = bridge.copy_value,
    encode = bridge.encode,
    request = function(source: core.Node): any
        if source.kind == "resource" then
            return { resources = { bridge.type_descriptor(source.token) }, optional = rawget(source :: any, "optional") or false }
        end
        local types = {}
        for _, name in source.names do table.insert(types, bridge.type_descriptor(source.tokens[name])) end
        return { components = types }
    end,
    sample = function(requests: {any})
        -- An empty request reads only frame metadata and keeps constant-only views valid.
        if #requests == 0 then requests = { {} } end
        return scheduler.host("game.snapshot", { batch = requests })
    end,
    decode = function(source: core.Node, result: any): any
        if source.kind == "resource" then
            local text = result.resources[1]
            if text == "null" and rawget(source :: any, "optional") then return nil end
            return bridge.decode_snapshot(source.token, text)
        end
        local rows: {any} = core.array()
        for _, entity in result.entities do
            local row: {[string]: any} = { entity = entity.entity }
            for index, name in source.names do
                row[name] = bridge.decode_snapshot(source.tokens[name], entity.components[index])
            end
            table.insert(rows, row)
        end
        return rows
    end,
}
