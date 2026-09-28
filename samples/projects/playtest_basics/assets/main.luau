export type PlayerState = { x: f32, steps: i32 }
export type Marker = { value: i32 }

local function initialize(commands: Commands)
    commands:spawn(Marker { value = 7 })
    commands:spawn(Camera2d.new { vertical_size = 6.0 }, Transform2d.new())
end

local function update(player: ResRW<PlayerState>, keys: ResRO<KeyInput>)
    player.steps += 1
    if keys:pressed(KeyCode.D) then player.x += 1 end
end

export local GamePlugin = Plugin.new {
    build = function(app: App)
        app:add_resource(PlayerState {})
        app:add_system(StartUp, initialize)
        app:add_system(FixedUpdate, update)
    end,
}
