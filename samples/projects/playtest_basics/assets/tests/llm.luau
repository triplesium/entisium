local ai = require("@ai")
local schema = require("@schema")
local context = require("@context")
local game = require("@playtest/game")
local task = require("@task")
local Gameplay = require("../main")

local view = context.view {
    description = "Holding D for one step increases x by 1. Reach x = 3.",
    fields = { player = context.resource(Gameplay.PlayerState) },
}
local planner = ai.model {
    model = "planner",
    instructions = "Choose right when data.player.x is below 3, otherwise stay.",
    output = schema.object {
        action = schema.enum { "right", "stay" },
        reason = schema.string():describe("Brief reason for the action"),
        target = schema.optional(schema.integer()):describe("Optional target x coordinate"),
    },
}
return function()
    game.start()
    task.on_cleanup(game.input.release_all)
    for _ = 1, 6 do
        local sample = view:sample()
        local result = task.timeout(planner:generate {
            context = sample, prompt = "Choose the next action to reach x = 3.",
        }, 20):await()
        assert(view:sample().frame == sample.frame, "Model waiting must pause simulation")
        if result.data.action == "stay" then
            assert(sample.data.player.x == 3, "Stopped before reaching the target")
            local narrator = ai.model { model = "planner" }
            local summary = task.timeout(narrator:generate {
                context = view:sample(), prompt = "Describe the final player position in one sentence.",
            }, 20):await()
            assert(type(summary.data) == "string" and #summary.data > 0)
            return
        end
        game.input.press("D")
        game.next_step():await()
        game.input.release("D")
    end
    error("Model did not reach the target within the decision budget")
end
