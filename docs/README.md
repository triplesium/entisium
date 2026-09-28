# Documentation

This directory contains both usage guides and implementation notes. Start with the root [README](../README.md) for build requirements and a native quick start.

## Guides

| Topic | Use it when |
| --- | --- |
| [Browser and Web Editor](browser.md) | Building the WebAssembly targets, running browser smoke tests, editing local projects, or connecting to the Editor MCP endpoint |
| [Repository structure](repository-structure.md) | Understanding the Agent, DevKit, Editor and build boundaries |
| [Entisium Agent](agent.md) | Running the shared Agent in the Editor or CLI and controlling native runtimes |
| [Playtest](playtest.md) | Declaring a deterministic game-control contract or driving a project through Editor MCP |
| [Luau tasks](luau-task.md) | Scheduling coroutines, awaiting results, and implementing host adapters |
| [Luau JSON](luau-json.md) | Encoding JSON, preserving null and empty arrays, and validating decoded data |
| [Luau AI](luau-ai.md) | Configuring aliases and using native Jev or ordinary LLM requests |
| [Luau HTTP](luau-http.md) | Native asynchronous HTTP requests and game frame integration |
| [Luau schemas](luau-schema.md) | Defining typed data schemas and validating values |
| [Luau context views](luau-context.md) | Building typed observation graphs and sampling game state |
| [2D physics](physics2d.md) | Adding Box2D bodies, collision layers, sensors, interpolation, or teleports |
| [Profiling](profiling.md) | Capturing bounded CPU summaries or inspecting Tracy zones |

## Engine notes

These pages document current internal contracts. They are useful when changing the corresponding subsystem, but they are not a promise of stable public API.

| Topic | Scope |
| --- | --- |
| [Luau libraries](luau-libraries.md) | Shared library ownership, native bindings, SDK publication and Runtime boundaries |
| [ECS behavior](ecs.md) | Resource access, fixed schedules, and removed-component readers |
| [Render App](render-app.md) | Main/Render World ownership, extraction, threading, and backend bootstrap |
| [Shader compilation](shader-compilation.md) | Slang targets, resource-name mapping, and shader artifact caching |

## Keeping documentation current

When behavior changes:

1. Update the narrowest relevant guide in the same change.
2. Keep commands runnable from the repository root unless the text says otherwise.
3. Link to source or tests instead of duplicating volatile implementation details across several pages.
4. Add new pages to this index and link major user-facing workflows from the root README.
5. Do not document generated files under `build/` as source-controlled inputs.
