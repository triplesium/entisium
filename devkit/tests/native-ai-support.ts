import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { once } from "node:events";
import { existsSync } from "node:fs";
import { mkdtemp, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { runLuau } from "../src/luau/host.js";
import { type NativeHostConfiguration } from "../src/luau/configuration.js";
export const executable = resolve("../build/windows/x64/release/entisium-luau-host.exe");
export const runtimeExecutable = resolve("../build/windows/x64/release/entisium-runtime-host.exe");
export const available = existsSync(executable);
export async function serverFor(handler: (body: any, request: IncomingMessage, response: ServerResponse) => unknown | Promise<unknown>) {
    let failure: unknown;
    let calls = 0;
    const server = createServer(async (request, response) => {
        try {
            calls++;
            let text = "";
            for await (const chunk of request) text += chunk.toString();
            const result = await handler(JSON.parse(text), request, response);
            if (!response.writableEnded) { response.setHeader("Content-Type", "application/json"); response.end(JSON.stringify(result)); }
        } catch (error) { failure = error; response.statusCode = 500; response.end("test handler failed"); }
    });
    server.listen(0, "127.0.0.1"); await once(server, "listening");
    const address = server.address();
    if (!address || typeof address === "string") throw new Error("Missing loopback port");
    return {url: `http://127.0.0.1:${address.port}/v1`, calls: () => calls, async close() {
        server.closeAllConnections(); await new Promise<void>(resolve => server.close(() => resolve()));
        if (failure) throw failure;
    }};
}
export function configuration(url: string, protocol: "jev" | "responses" | "chat-completions" = "chat-completions"): NativeHostConfiguration {
    return {version: 1, connections: {test: {base_url: url, headers: [{name: "Authorization", value: "Bearer native-test-secret"}]}},
        generation: protocol === "jev" ? {} : {planner: {id: "requested-model", protocol, connection: "test", timeout: 2, max_tokens: 512}},
        decisions: protocol !== "jev" ? {} : {"fast-decision": {id: "jev-latest", protocol, connection: "test", timeout: 2}},
    };
}
export async function runNative(script: string, config: NativeHostConfiguration) {
    const root = await mkdtemp(join(tmpdir(), "entisium-native-ai-"));
    try {
        const entry = join(root, "test.luau");
        await writeFile(entry, `local ai = require("@ai")\nlocal schema = require("@schema")\nlocal task = require("@task")\nlocal http = require("@http")\nreturn {run = function()\n${script}\nend}`);
        await runLuau({executable, sourceRoot: root, entry, sdkDirectory: resolve("../runtime/playtest/luau"), configuration: config,
            async dispatch() { throw new Error("AI must not call Node services"); },
            async dispatchTask() { throw new Error("AI must not call Node background services"); },
        });
    } finally { await rm(root, {recursive: true, force: true}); }
}
export function chatResponse(content: string, extra: Record<string, unknown> = {}) {
    return {model: "actual-model", choices: [{message: {role: "assistant", content}, finish_reason: "stop", ...extra}], usage: {prompt_tokens: 12, completion_tokens: 3}};
}
export function responsesResponse(content: string, extra: Record<string, unknown> = {}) {
    return {model: "actual-model", status: "completed", output: [{type: "message", role: "assistant", status: "completed", content: [{type: "output_text", text: content}]}], usage: {input_tokens: 12, output_tokens: 3}, ...extra};
}
