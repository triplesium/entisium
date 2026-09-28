import { existsSync } from "node:fs";
import { mkdtemp, readFile, rm, cp, mkdir, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { expect, it, vi } from "vitest";
import { runPlaytest } from "../src/playtest/runner.js";
import { executable, runtimeExecutable, serverFor, chatResponse } from "./native-ai-support.js";

it.skipIf(!existsSync(runtimeExecutable) || !existsSync(executable)).each(["jev", "chat-completions"] as const)(
    "loads YAML in both hosts and records/replays the native %s controller", async protocol => {
        const directory = await mkdtemp(join(tmpdir(), "entisium-native-model-loop-"));
        const server = await serverFor((body, request) => {
            expect(request.headers.authorization).toBe("Bearer native-test-secret");
            if (protocol === "jev") {
                const right = body.state.data.player.x < 3;
                return {model: "mock-jev", answers: {movement: {type: "choice", choice: right ? "right" : "stay", confidence: 1,
                    probabilities: {right: right ? 1 : 0, stay: right ? 0 : 1}}}};
            }
            if (!body.response_format) return chatResponse("The player reached x = 3.");
            const context = JSON.parse(body.messages[1].content.split("\n").slice(1).join("\n"));
            return chatResponse(JSON.stringify({data: {action: context.data.player.x < 3 ? "right" : "stay", reason: "Reach target", target: null}}));
        });
        try {
            await cp(resolve("../samples/projects/playtest_basics"), directory, {recursive: true});
            await mkdir(join(directory, ".entisium"), {recursive: true});
            const configPath = join(directory, ".entisium/config.yaml");
            vi.stubEnv("ETS_CONFIG_PATH", configPath);
            const writeConfig = async (baseUrl: string) => writeFile(configPath, JSON.stringify({version: 1,
                providers: {test: {type: "openai-compatible", apiKey: "native-test-secret", chat: {api: "chat-completions", baseUrl}, decisions: {baseUrl}}},
                decisions: {models: {"fast-decision": {provider: "test", id: "jev-latest"}}},
                llm: {models: {planner: {provider: "test", id: "requested-model"}}},
            }));
            await writeConfig(server.url);
            const main = join(directory, "assets/main.luau");
            await writeFile(main, 'local ai = require("@ai")\nlocal configured = ai.model {model="planner"}\n' + await readFile(main, "utf8"));
            const options = {project: join(directory, "project.yaml"), entry: join(directory, `assets/tests/${protocol === "jev" ? "decision" : "llm"}.luau`), runtimeExecutable, luauExecutable: executable};
            const path = join(directory, "tape.json");
            const recorded = await runPlaytest({...options, modelTape: {mode: "record", path}});
            expect(recorded.frame).toBe(4);
            const expected = protocol === "jev" ? 4 : 5;
            expect(server.calls()).toBe(expected);
            const text = await readFile(path, "utf8");
            const tape = JSON.parse(text);
            expect(tape.entries).toHaveLength(expected);
            expect(tape.format).toBe("entisium.http-tape");
            expect(text).not.toContain("native-test-secret");
            // A changed, unreachable endpoint proves replay never goes to the network.
            await writeConfig("http://127.0.0.1:9/v1");
            const replayed = await runPlaytest({...options, modelTape: {mode: "replay", path}});
            expect(replayed).toEqual(recorded);
            expect(server.calls()).toBe(expected);
            tape.entries[0].request.body = "mismatch";
            const badPath = join(directory, "bad-tape.json"); await writeFile(badPath, JSON.stringify(tape));
            await expect(runPlaytest({...options, modelTape: {mode: "replay", path: badPath}})).rejects.toThrow("HTTP replay request mismatch");
        } finally {
            vi.unstubAllEnvs(); await server.close(); await rm(directory, {recursive: true, force: true});
        }
    }, 30_000,
);
