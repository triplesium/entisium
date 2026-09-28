import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { afterEach, expect, it, vi } from "vitest";
import { YamlConfigStore, defaultConfigPath, loadConfig } from "../src/settings/yaml-store.js";
import { runtimeExecutableFromConfig } from "../src/settings/runtime.js";

const directories: string[] = [];
afterEach(async () => {
    vi.unstubAllEnvs();
    for (const path of directories.splice(0)) await rm(path, { recursive: true, force: true });
});
async function fixture(source: string) {
    const root = await mkdtemp(join(tmpdir(), "entisium-config-test-"));
    directories.push(root);
    const path = join(root, "config.yaml");
    await writeFile(path, source);
    return new YamlConfigStore(path);
}

it("reads YAML providers with inline keys and applies DevKit defaults", async () => {
    const store = await fixture("version: 1\nproviders:\n  openai:\n    type: openai\n    apiKey: test-api-key\n");
    const config = await store.read();
    expect(config.providers.openai.apiKey).toBe("test-api-key");
    expect(config.imageGeneration).toMatchObject({ model: { provider: "openai", id: "gpt-image-2" }, defaults: {} });
    expect(store.resolvePath("bin/runtime.exe")).toBe(resolve(store.path, "..", "bin/runtime.exe"));
    vi.stubEnv("ETS_RUNTIME_HOST_PATH", "");
    config.runtime.executable = "bin/runtime.exe";
    expect(runtimeExecutableFromConfig(config, store)).toBe(store.resolvePath(config.runtime.executable));
    vi.stubEnv("ETS_RUNTIME_HOST_PATH", "env/runtime.exe");
    expect(runtimeExecutableFromConfig(config, store)).toBe(resolve("env/runtime.exe"));
});

it("serializes updates without losing unrelated sections or their comments", async () => {
    const store = await fixture("# user settings\nversion: 1\nproviders: {}\n# keep runtime\nruntime:\n  executable: ./runtime.exe\neditor:\n  custom: keep\n");
    await Promise.all([
        store.update((config) => { config.providers.openai = { type: "openai", apiKey: "test-api-key" }; }),
        store.update((config) => { config.agent = { reasoning: "high" }; }),
    ]);
    const result = await store.read();
    expect(result.providers.openai.apiKey).toBe("test-api-key");
    expect(result.agent).toEqual({ reasoning: "high" });
    expect(result.editor).toEqual({ custom: "keep" });
    expect(await readFile(store.path, "utf8")).toContain("# keep runtime");
});

it("rejects invalid syntax, duplicate keys, versions and values without echoing secrets", async () => {
    for (const source of [
        "version: 1\nproviders: [secret-do-not-print",
        "version: 1\nversion: 1\n# secret-do-not-print",
        "version: secret-do-not-print\n",
        "version: 1\nproviders:\n  openai:\n    type: openai\n    apiKey: 'secret-do-not-print with space'\n",
        "version: 1\nimageGeneration:\n  timeoutMs: secret-do-not-print\n",
    ]) {
        const store = await fixture(source);
        const error = await store.read().catch((error: Error) => error);
        expect(error).toBeInstanceOf(Error);
        expect((error as Error).message).not.toContain("secret-do-not-print");
    }
});

it("rejects invalid updates without changing the file and distinguishes missing files", async () => {
    const store = await fixture("version: 1\n");
    await expect(store.update((config) => { config.runtime.executable = ""; })).rejects.toThrow();
    expect(await readFile(store.path, "utf8")).toBe("version: 1\n");
    expect(await new YamlConfigStore(`${store.path}.missing`).readOptional()).toBeUndefined();
    vi.stubEnv("ETS_CONFIG_PATH", store.path);
    expect(defaultConfigPath()).toBe(store.path);
});

it("selects explicit, environment, project and user files in order without merging", async () => {
    const explicit = await fixture("version: 1\nimageGeneration: { model: { provider: openai, id: explicit } }\n");
    const environment = await fixture("version: 1\nimageGeneration: { model: { provider: openai, id: environment } }\n");
    const root = dirname(explicit.path);
    const project = join(root, "project");
    const local = join(project, ".entisium", "config.yaml");
    await mkdir(dirname(local), { recursive: true });
    await writeFile(local, "version: 1\nimageGeneration: { model: { provider: openai, id: local } }\n");
    vi.stubEnv("ETS_CONFIG_PATH", "");
    vi.stubEnv("APPDATA", join(root, "user"));
    vi.stubEnv("XDG_CONFIG_HOME", join(root, "user"));
    const userPath = defaultConfigPath();
    await mkdir(dirname(userPath), { recursive: true });
    await writeFile(userPath, "version: 1\nimageGeneration: { model: { provider: openai, id: user }, defaults: { quality: high } }\n");
    vi.stubEnv("ETS_CONFIG_PATH", environment.path);
    expect((await loadConfig(explicit.path, project)).config?.imageGeneration.model.id).toBe("explicit");
    expect((await loadConfig(undefined, project)).config?.imageGeneration.model.id).toBe("environment");
    vi.stubEnv("ETS_CONFIG_PATH", "");
    const selected = await loadConfig(undefined, project);
    expect(selected.store.path).toBe(local);
    expect(selected.config?.imageGeneration).toMatchObject({ model: { provider: "openai", id: "local" }, defaults: {} });
    await rm(local);
    expect((await loadConfig(undefined, project)).store.path).toBe(userPath);
    await rm(userPath);
    expect((await loadConfig(undefined, project)).config).toBeUndefined();
});

it("does not fall back when a selected project file is invalid or an explicit file is missing", async () => {
    const root = dirname((await fixture("version: 1\n")).path);
    await mkdir(join(root, ".entisium"));
    await writeFile(join(root, ".entisium", "config.yaml"), "version: broken\n");
    vi.stubEnv("ETS_CONFIG_PATH", "");
    await expect(loadConfig(undefined, root)).rejects.toThrow("Invalid config.yaml");
    await expect(loadConfig(join(root, "missing.yaml"), root)).rejects.toThrow("does not exist");
});

it("defaults to OpenAI Images and rejects unknown image protocols", async () => {
    const store = await fixture("version: 1\n");
    expect((await store.read()).imageGeneration.model.provider).toBe("openai");
    await writeFile(store.path, "version: 1\nimageGeneration:\n  api: unsupported\n");
    await expect(store.read()).rejects.toThrow("imageGeneration");
});

it("accepts the fal.ai image protocol and GPT Image 2.5 quality levels", async () => {
    const store = await fixture("version: 1\nproviders:\n  fal: { type: fal }\nimageGeneration:\n  model: { provider: fal, id: openai/gpt-image-2.5/sunburst/text-to-image }\n  defaults: { quality: max }\n");
    expect((await store.read()).imageGeneration).toMatchObject({
        model: { provider: "fal", id: "openai/gpt-image-2.5/sunburst/text-to-image" }, defaults: { quality: "max" },
    });
});
