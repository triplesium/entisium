--!strict
local bridge = require("@context/bridge")
local task = require("@task")
export type PlaytestGame = {
    start: () -> (),
    stop: () -> (),
    capture: () -> { frame: number, image: { content_type: string, encoding: string, data: string } },
    read_resource: <T>(PlaytestGame, TypeToken<T>) -> T,
    query: (<T>(PlaytestGame, TypeToken<T>) -> {{ entity: number, components: {T} }})
        & ((PlaytestGame, ...TypeToken<any>) -> {{ entity: number, components: {any} }}),
    input: {
        press: (string) -> (),
        release: (string) -> (),
        release_all: () -> (),
    },
    delay: (number) -> task.Task<nil>,
    next_step: () -> task.Task<nil>,
    when: (() -> boolean) -> task.Task<nil>,
}


local scheduler = require("@playtest/scheduler")
local keys: {[string]: boolean} = {}
local game = {}

local function freeze<T>(value: T): T
    if type(value) == "table" then
        for _, child in value :: any do freeze(child) end
        table.freeze(value)
    end
    return value
end

function game.start(): () scheduler.host("game.start") end
function game.stop(): () scheduler.host("game.stop") end
function game.capture(): {frame: number, image: {content_type: string, encoding: string, data: string}} return freeze(scheduler.host("game.capture")) end

function game.read_resource<T>(_self: PlaytestGame, token: TypeToken<T>): T
    local value = scheduler.host("game.snapshot", {
        resources = { bridge.type_descriptor(token) },
    }).resources[1]
    return bridge.decode_snapshot(token, value)
end

-- Each row contains an entity identifier and components in requested order.
function game.query(_self: PlaytestGame, ...: TypeToken<any>): {{entity: number, components: {any}}}
    local tokens = { ... }
    local types = {}
    for _, token in tokens do table.insert(types, bridge.type_descriptor(token)) end
    assert(#types > 0, "query requires at least one component type")
    local entities = scheduler.host("game.snapshot", { components = types }).entities
    for _, row in entities do
        for index, token in tokens do
            row.components[index] = bridge.decode_snapshot(token, row.components[index])
        end
    end
    return freeze(entities)
end

local function sync(): ()
    local held = {}
    for key in keys do table.insert(held, key) end
    table.sort(held)
    scheduler.host("game.input", { keys = held })
end
game.input = {}
function game.input.press(key: string): () keys[key] = true; sync() end
function game.input.release(key: string): () keys[key] = nil; sync() end
function game.input.release_all(): () table.clear(keys); sync() end
game.delay = scheduler.delay
game.when = scheduler.when
game.next_step = scheduler.next_step
local public: PlaytestGame = game
return public
