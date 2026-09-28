import { mkdtemp, mkdir, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { expect, it, vi } from "vitest";
import { configuredLuauHost, emptyHostConfiguration } from "../src/luau/configuration.js";
import { available, runNative, configuration } from "./native-ai-support.js";
it("resolves native model aliases, provider types and credential precedence without network", async () => {
    const directory = await mkdtemp(join(tmpdir(), "entisium-native-config-"));
    try {
        const path = join(directory, ".entisium/config.yaml"); await mkdir(join(directory, ".entisium"));
        vi.stubEnv("ETS_CONFIG_PATH", path); vi.stubEnv("TYPESAFE_API_KEY", "environment-test-key");
        await writeFile(path, JSON.stringify({version: 1, providers: {
            custom: {type: "typesafe"}, chat: {type: "openai", apiKey: "inline-test-key"},
        }, decisions: {models: {fast: {provider: "custom", id: "jev-pinned"}}}, llm: {models: {planner: {provider: "chat", id: "text-pinned"}}}}));
        const resolve = vi.fn(async () => undefined);
        const config = await configuredLuauHost(directory, resolve);
        expect(config.decisions.fast).toMatchObject({id: "jev-pinned", protocol: "jev", connection: "decisions:custom"});
        expect(config.connections["decisions:custom"].base_url).toBe("https://api.typesafe.ai/v1");
        expect(config.connections["decisions:custom"].headers[0].value).toBe("Bearer environment-test-key");
        expect(config.connections["generation:chat"].headers[0].value).toBe("Bearer inline-test-key");
        expect(resolve).toHaveBeenCalledTimes(1);
        expect(config.generation.planner.protocol).toBe("responses");
        const controller = new AbortController(); controller.abort();
        await expect(configuredLuauHost(directory, undefined, controller.signal)).rejects.toThrow();
    } finally { vi.unstubAllEnvs(); await rm(directory, {recursive: true, force: true}); }
});
it.skipIf(!available)("reports missing configuration", async () => {
    await runNative(`local ok, error = pcall(ai.model, {model="missing"})
        assert(not ok and string.find(error, "configuration is missing", 1, true))`, emptyHostConfiguration());
});

it.skipIf(!available)("reports missing credentials before making a request", async () => {
    const config = configuration("http://127.0.0.1:9/v1");
    config.connections.test.headers = [];
    config.connections.test.error = "Model provider credential is missing";
    await runNative(`local ok, message = pcall(ai.model, {model="planner"})
        assert(not ok and string.find(message, "credential is missing", 1, true))`, config);
});
