#pragma once

#include "app/app.hpp"
#include "app/plugin.hpp"
#include "asset/plugin.hpp"
#include "scripting/asset.hpp"
#include "scripting/host_config.hpp"

namespace ets {

ETS_REFLECT(Plugin)
class LuauScriptingPlugin : public Plugin {
    std::shared_ptr<const LuauHostConfig> m_config;

  public:
    explicit LuauScriptingPlugin(
        std::shared_ptr<const LuauHostConfig> config = {}
    ) : m_config(std::move(config)) {}
    void dependencies(PluginDependencies& dependencies) const override {
        dependencies
            .require<AssetPlugin<LuauScriptAsset, LuauScriptAssetLoader>>();
    }

    void setup(App& app) override;
};

} // namespace ets
