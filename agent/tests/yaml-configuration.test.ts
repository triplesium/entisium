import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, it, vi } from "vitest";
import { loadHostConfiguration } from "../src/host/configuration.js";
import { ModelMetadataService } from "@entisium/devkit/models/service";
import { HostModelRegistry } from "../src/models/model-registry.js";
import { createHostImageGeneration } from "../src/host/image-generation.js";
import type { HostProjectService } from "@entisium/devkit/workspace/project-service";

const directories: string[] = [];
afterEach(async () => {
    vi.unstubAllEnvs();
    for (const path of directories.splice(0)) await rm(path, { recursive: true, force: true });
});
const source = `version: 1
providers:
  openai:
    type: openai
    apiKey: test-yaml-secret
    models:
      - id: test-chat
        reasoning: true
        contextWindow: 128000
        maxTokens: 4096
agent:
  model: { provider: openai, id: test-chat }
  reasoning: high
imageGeneration:
  model: { provider: openai, id: test-image }
  defaults: { size: 1536x1024, quality: low }
runtime:
  executable: auto
`;
async function fixture(text = source) {
    const root = await mkdtemp(join(tmpdir(), "entisium-agent-config-test-"));
    directories.push(root);
    const path = join(root, "config.yaml");
    await writeFile(path, text);
    return { path, host: await loadHostConfiguration(path) };
}

it("uses YAML for model selection and credentials without exposing keys in registry snapshots", async () => {
    const { host } = await fixture();
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    expect((await registry.activeModel()).model.id).toBe("test-chat");
    expect(host.agent.reasoning).toBe("high");
    expect(await host.credentials.read("openai")).toEqual({ type: "api_key", key: "test-yaml-secret" });
    expect(JSON.stringify(await registry.snapshot())).not.toContain("test-yaml-secret");
    expect(JSON.stringify(await host.modelSettingsStore.read())).not.toContain("apiKey");
});

it("writes model and key edits into the same YAML while preserving DevKit configuration", async () => {
    const { host, path } = await fixture();
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    await registry.configure({ providerId: "openai", modelId: "test-chat", apiKey: "updated-yaml-key" });
    await registry.configureProvider({ provider: { id: "custom", name: "Custom", api: "responses", baseUrl: "https://example.com/v1" }, apiKey: "custom-test-key" });
    const updated = await host.store.read();
    expect(updated.providers.openai.apiKey).toBe("updated-yaml-key");
    expect(updated.providers.custom.apiKey).toBe("custom-test-key");
    expect(updated.providers.custom.type).toBe("openai-compatible");
    expect(updated.imageGeneration.model.id).toBe("test-image");
    expect(updated.agent).toMatchObject({ reasoning: "high" });
    expect(await readFile(path, "utf8")).toContain("updated-yaml-key");
    await registry.deleteCredential("openai");
    expect(await host.credentials.read("openai")).toBeUndefined();
    expect(await host.credentials.list()).toEqual([{ providerId: "custom", type: "api_key" }]);
});

it("validates Agent configuration and explicitly selected paths instead of falling back", async () => {
    const { path } = await fixture();
    await expect(loadHostConfiguration(`${path}.missing`)).rejects.toThrow("does not exist");
    await writeFile(path, source.replace("reasoning: high", "reasoning: secret-invalid-value"));
    await expect(loadHostConfiguration(path)).rejects.toThrow("agent settings");
    await writeFile(path, source.replace("provider: openai, id: test-chat", "provider: missing, id: test-chat"));
    await expect(loadHostConfiguration(path)).rejects.toThrow("selected provider");
});

it("rejects credentials for unconfigured providers without changing YAML", async () => {
    const { host, path } = await fixture();
    const before = await readFile(path, "utf8");
    await expect(host.credentials.modify("missing", async () => ({ type: "api_key", key: "test-missing-key" })))
        .rejects.toThrow("Configure the provider and its type");
    expect(await readFile(path, "utf8")).toBe(before);
});

it("gives explicit paths priority over ETS_CONFIG_PATH", async () => {
    const { path } = await fixture();
    vi.stubEnv("ETS_CONFIG_PATH", `${path}.missing`);
    expect((await loadHostConfiguration(path)).config?.version).toBe(1);
    await expect(loadHostConfiguration()).rejects.toThrow("does not exist");
});

it("can delete the active model when an image-only provider comes first", async () => {
    const reordered = source.replace('  openai:\n', '  images:\n    type: openai-compatible\n    images: { baseUrl: https://api.openai.com/v1 }\n    apiKey: image-only-key\n  openai:\n');
    const { host } = await fixture(reordered);
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    await registry.configureRegistryModel({ providerId: "openai", model: {
        id: "other-chat", name: "Other chat", reasoning: false, contextWindow: 128000, maxTokens: 4096,
    } });
    expect((await registry.deleteModel("openai", "test-chat")).active).toEqual({ providerId: "openai", modelId: "other-chat" });
});

it("selects arbitrary model IDs without creating a catalogue when saved or reloaded", async () => {
    const { host, path } = await fixture(`version: 1
providers:
  router: { type: openrouter, apiKey: shared-router-key }
  fal: { type: fal, apiKey: independent-fal-key }
agent:
  model: { provider: router, id: vendor/new-model }
imageGeneration:
  model: { provider: fal, id: openai/gpt-image-2.5/sunburst/text-to-image }
`);
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    expect((await registry.activeModel()).model).toMatchObject({
        id: "vendor/new-model", provider: "router", contextWindow: 32768, maxTokens: 4096, reasoning: false,
    });
    expect((await registry.snapshot()).providers.map((provider) => provider.id)).toEqual(["router"]);
    expect(() => createHostImageGeneration({} as HostProjectService, host.credentials, host.config)).not.toThrow();
    await registry.configure({ providerId: "router", modelId: "vendor/next-model" });
    await registry.configureProvider({ provider: { id: "router", name: "Router", api: "responses", baseUrl: "https://chat.example/v1" } });
    const saved = await host.store.read();
    expect(saved.providers.router).not.toHaveProperty("models");
    expect(saved.providers.router.type).toBe("openrouter");
    expect(saved.providers.fal).toEqual({ type: "fal", apiKey: "independent-fal-key" });
    expect(saved.imageGeneration.model.provider).toBe("fal");
    const reloaded = await loadHostConfiguration(path);
    const next = new HostModelRegistry(reloaded.credentials, reloaded.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    expect((await next.activeModel()).model.id).toBe("vendor/next-model");
    expect((await next.getModel("router", "vendor/cli-only"))?.id).toBe("vendor/cli-only");
    expect(await next.getModel("fal", "unsupported-chat")).toBeUndefined();
    expect(await next.getModel("missing", "any-model")).toBeUndefined();
});

it("keeps capability-specific connections and credentials when editing chat settings", async () => {
    const { host } = await fixture(`version: 1
providers:
  shared:
    type: openrouter
    apiKey: shared-api-key
    images: { api: openrouter-images, baseUrl: https://images.example/v1 }
  spare:
    type: openai-compatible
    chat: { baseUrl: https://spare.example/v1 }
    models: [{ id: spare-model, contextWindow: 8192, maxTokens: 1024 }]
agent:
  model: { provider: shared, id: new-model }
imageGeneration:
  model: { provider: shared, id: image-model }
`);
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    await registry.configureProvider({ provider: { id: "shared", name: "Shared", api: "responses", baseUrl: "https://chat.example/v1" } });
    expect((await host.store.read()).providers.shared).toMatchObject({
        type: "openrouter", apiKey: "shared-api-key", images: { api: "openrouter-images", baseUrl: "https://images.example/v1" },
    });
    await expect(registry.deleteProvider("shared")).rejects.toThrow("shared with image generation");
    expect((await host.store.read()).providers.shared.apiKey).toBe("shared-api-key");
    expect((await registry.activeModel()).model.id).toBe("new-model");
});

it("allows a provider without a catalogue or default selection to resolve a CLI model", async () => {
    const { host } = await fixture("version: 1\nproviders:\n  custom:\n    type: openai-compatible\n    chat: { baseUrl: https://custom.example/v1 }\n");
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    expect((await registry.getModel("custom", "new-model"))?.id).toBe("new-model");
    expect((await host.store.read()).providers.custom).not.toHaveProperty("models");
});

it("uses optional model metadata without restricting selection to that catalogue", async () => {
    const { host } = await fixture();
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: async () => new Response(null, { status: 503 }) }));
    expect((await registry.getModel("openai", "test-chat"))).toMatchObject({ reasoning: true, contextWindow: 128000 });
    await registry.configure({ providerId: "openai", modelId: "unlisted-chat" });
    expect((await registry.activeModel()).model.id).toBe("unlisted-chat");
    expect((await host.store.read()).providers.openai.models).toHaveLength(1);
});

it("resolves live chat metadata and preserves partial overrides without saving downloaded models", async () => {
    const { host } = await fixture(`version: 1
providers:
  router:
    type: openrouter
    apiKey: first-test-key
    models: [{ id: vendor/chat, reasoning: false }]
agent:
  model: { provider: router, id: vendor/chat }
`);
    const request = vi.fn(async (_url: unknown, init?: RequestInit) => new Response(JSON.stringify({ data: [
        { id: "vendor/chat", name: "Fetched chat", context_length: new Headers(init?.headers).get("Authorization")?.includes("second") ? 128000 : 64000,
            architecture: { input_modalities: ["text", "image"], output_modalities: ["text"] }, supported_parameters: ["reasoning", "tools"] },
        { id: "vendor/other", context_length: 100000, architecture: { output_modalities: ["text"] } },
        { id: "vendor/image", architecture: { output_modalities: ["image"] } },
    ] })));
    const service = new ModelMetadataService({ fetch: request, cacheDirectory: null });
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, service);
    const original = (await registry.activeModel()).model;
    expect(original).toMatchObject({ contextWindow: 64000, reasoning: false, input: ["text", "image"], maxTokens: 4096 });
    const listed = await registry.refreshModels("router");
    expect(listed.providers[0].models.map((model) => model.id)).toEqual(["vendor/chat", "vendor/other"]);
    expect(listed.providers[0].metadata?.["vendor/chat"].sources["chat.contextWindow"].source).toBe("remote");
    expect(request).toHaveBeenCalledTimes(1);
    await registry.configure({ providerId: "router", modelId: "vendor/other" });
    expect((await host.store.read()).providers.router.models).toEqual([{ id: "vendor/chat", reasoning: false }]);
    await host.credentials.modify("router", async () => ({ type: "api_key", key: "second-test-key" }));
    expect((await registry.getModel("router", "vendor/chat"))?.contextWindow).toBe(128000);
    expect(original.contextWindow).toBe(64000);
    expect(request).toHaveBeenCalledTimes(2);
    expect((await registry.snapshot()).providers[0].metadata?.["vendor/other"].sources["chat.contextWindow"]).toBeUndefined();
});

it("does not publish an old in-flight lookup after the provider connection changes", async () => {
    const { host } = await fixture("version: 1\nproviders:\n  router: { type: openrouter, apiKey: test-router-key }\nagent:\n  model: { provider: router, id: vendor/chat }\n");
    let complete!: (response: Response) => void;
    const request = vi.fn(() => new Promise<Response>((resolve) => { complete = resolve; }));
    const registry = new HostModelRegistry(host.credentials, host.modelSettingsStore, new ModelMetadataService({ cacheDirectory: null, fetch: request }));
    const original = registry.getModel("router", "vendor/chat");
    await vi.waitFor(() => expect(request).toHaveBeenCalledOnce());
    await registry.configureProvider({ provider: { id: "router", name: "New router", baseUrl: "https://new.example/v1", api: "chat-completions" } });
    complete(new Response(JSON.stringify({ data: [{ id: "vendor/chat", context_length: 90000 }] })));
    expect(await original).toMatchObject({ contextWindow: 90000, baseUrl: "https://openrouter.ai/api/v1" });
    const current = await registry.snapshot();
    expect(current.providers[0].models[0]).toMatchObject({ contextWindow: 32768, baseUrl: "https://new.example/v1" });
});

it.each(["llm", "decisions"] as const)("preserves providers referenced by %s models until reassigned", async (section) => {
    const { host, path } = await fixture(`version: 1
providers:
  custom:
    type: openai-compatible
    chat: {baseUrl: https://custom.example/v1}
  replacement:
    type: openai-compatible
    chat: {baseUrl: https://replacement.example/v1}
${section}:
  models:
    planner: {provider: custom, id: example}
`);
    const before = await readFile(path, "utf8");
    const settings = await host.modelSettingsStore.read();
    const remaining = { ...settings, active: undefined, providers: settings.providers.filter(provider => provider.id !== "custom") };
    await expect(host.modelSettingsStore.write(remaining)).rejects.toThrow(`${section}.models.planner`);
    expect(await readFile(path, "utf8")).toBe(before);
    await host.store.update(config => { config[section]!.models.planner.provider = "replacement"; });
    await host.modelSettingsStore.write(remaining);
    expect((await host.store.read()).providers).not.toHaveProperty("custom");
    expect((await host.store.read())[section]!.models.planner.provider).toBe("replacement");
});
