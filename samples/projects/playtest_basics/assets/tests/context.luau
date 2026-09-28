local context = require("@context")
local task = require("@task")
local game = require("@playtest/game")
local Gameplay = require("../main")
type MarkerRow = { entity: number, marker: Gameplay.Marker }

local player = context.resource(Gameplay.PlayerState)
local markers = context.query { marker = Gameplay.Marker }
local view = context.view {
    description = "Movement test; x increases when D is held",
    fields = {
        player = context.combine {
            x = player:map(function(p: Gameplay.PlayerState): number return p.x end):describe("Horizontal position"),
            steps = player:map(function(p: Gameplay.PlayerState): number return p.steps end),
        },
        markers = markers:filter(function(row: MarkerRow): boolean return row.marker.value > 0 end)
            :order_by(function(row: MarkerRow): number return row.marker.value end, "desc")
            :take(8):map(function(row: MarkerRow): { entity: number, value: number }
                return { entity = row.entity, value = row.marker.value }
            end),
        count = markers:count(),
        raw = player,
    },
}

return function()
    game.start()
    task.on_cleanup(game.input.release_all)
    local before = view:sample()
    assert(before.data.count == 1 and before.data.markers[1].value == 7)
    assert(not pcall(function() before.data.player.x = 99 end))
    game.input.press("D")
    game.next_step():await()
    local after = view:sample()
    assert(after.frame == before.frame + 1)
    assert(after.data.player.x == before.data.player.x + 1)
    assert(after.data.raw.x == after.data.player.x)
    assert(after.simulation_time > before.simulation_time)
    assert(after.schema.fields.player.fields.x.description == "Horizontal position")
    print(after:encode())
end
