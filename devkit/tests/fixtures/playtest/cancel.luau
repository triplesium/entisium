local task = require("@task")
local game = require("@playtest/game")
return function()
    task.on_cleanup(game.input.release_all)
    game.delay(10):await()
end
