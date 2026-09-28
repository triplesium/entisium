#pragma once

#include "app/app.hpp"
#include "project/project.hpp"
#include "scripting/host_config.hpp"

namespace ets::runtime_host {

struct RuntimeHostOptions {
    bool hidden {false};
    std::shared_ptr<const LuauHostConfig> luau_config;
};

class RuntimeHostApplication {
  public:
    explicit RuntimeHostApplication(
        Project project,
        RuntimeHostOptions options = {}
    );
    RuntimeHostApplication(const RuntimeHostApplication&) = delete;
    RuntimeHostApplication& operator=(const RuntimeHostApplication&) = delete;
    RuntimeHostApplication(RuntimeHostApplication&&) = delete;
    RuntimeHostApplication& operator=(RuntimeHostApplication&&) = delete;
    ~RuntimeHostApplication();

    void run();

  private:
    void shutdown() noexcept;
    void update_frame();

    App m_app;
};

} // namespace ets::runtime_host
