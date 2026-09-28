local context = require("@context")
local ai = require("@ai")
local game = require("@playtest/game")
local task = require("@task")
local Gameplay = require("../main")

local view = context.view {
    description = "Holding D for one step increases x by 1. Reach x = 3.",
    fields = { player = context.resource(Gameplay.PlayerState) },
}
local policy = ai.decision {
    model = "fast-decision",
    questions = { movement = ai.choice {
        instructions = "Choose right if data.player.x is below 3; otherwise choose stay.",
        criteria = { right = "Hold D for one step", stay = "Release D" },
    } },
}
return function()
    game.start()
    task.on_cleanup(game.input.release_all)
    for _ = 1, 6 do
        local before = view:sample()
        local result = task.timeout(policy:evaluate(before), 10):await()
        assert(view:sample().frame == before.frame, "Model waiting must pause simulation")
        local movement = result.answers.movement
        assert(movement.type == "choice")
        if movement.choice == "stay" then
            assert(before.data.player.x == 3, "Model stopped before reaching the target")
            return
        end
        game.input.press("D")
        game.next_step():await()
        game.input.release("D")
    end
    error("Model did not reach the target within the decision budget")
end
