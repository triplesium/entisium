# Scripting examples

Examples stay with their runnable projects so scripts, project configuration and
assets can be used together. Run these commands from the repository root.

| Example | Project and scripts |
| --- | --- |
| ECS systems and UI | [Scripting sample](../../samples/projects/scripting/assets/scripts/) |
| Tasks, typed snapshots and input | [Playtest basics](../../samples/projects/playtest_basics/) |
| Schema validation | [Schema test](../../samples/projects/playtest_basics/assets/tests/schema.luau) |
| Context graphs | [Context test](../../samples/projects/playtest_basics/assets/tests/context.luau) |
| AI decisions | [Decision test](../../samples/projects/playtest_basics/assets/tests/decision.luau) |
| LLM generation | [LLM test](../../samples/projects/playtest_basics/assets/tests/llm.luau) |
| Complete game | [Skyline Strike](../../samples/projects/skyline_strike/) |
| Browser project | [Browser sample](../../samples/browser_project/project/) |

Build the native hosts and run the movement test:

```sh
xmake build -y entisium-runtime-host entisium-luau-host
npm run playtest -- samples/projects/playtest_basics/project.yaml samples/projects/playtest_basics/assets/tests/movement.luau
```

See [Playtest](../docs/playtest.md) for executable overrides and test authoring,
and [AI configuration](../docs/luau-ai.md) before running the AI examples.
