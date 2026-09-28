export type State = { value: i32 }
export local Uninstalled = Plugin.new {
    build = function(app: App)
        error("Importing this module must not install its Plugin")
    end,
}
