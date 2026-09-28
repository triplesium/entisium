local task = require("@task")
local game = require("@playtest/game")
local Gameplay = require("../main")

return function()
    game.start()
    task.on_cleanup(game.input.release_all)
    local initial = game:read_resource(Gameplay.PlayerState)
    local markers = game:query(Gameplay.Marker)
    assert(#markers == 1 and markers[1].components[1].value == 7)

    game.input.press("D")
    for i = 1, 4 do
        game.next_step():await()
        local state = game:read_resource(Gameplay.PlayerState)
        assert(state.steps == initial.steps + i)
        assert(state.x == initial.x + i)
    end
    game.input.release("D")

    local waits = task.all {
        duration = game.delay(0.1),
        condition = game.when(function()
            return game:read_resource(Gameplay.PlayerState).steps >= initial.steps + 7
        end),
    }
    waits:await()
    local final = game:read_resource(Gameplay.PlayerState)
    assert(final.steps == initial.steps + 10)
    assert(final.x == initial.x + 4)
    assert(initial.x == 0, "Old snapshots must not change")
end
