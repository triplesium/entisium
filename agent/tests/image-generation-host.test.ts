import { parseConfig } from "@entisium/devkit/settings/config";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import type { CredentialStore } from "@earendil-works/pi-ai";
import { createHostImageGeneration } from "../src/host/image-generation.js";
import { ImageGenerationService } from "@entisium/devkit/image-generation/service";
import type { HostProjectService } from "@entisium/devkit/workspace/project-service";

vi.mock("@entisium/devkit/image-generation/service", () => ({
    ImageGenerationService: vi.fn(class { constructor(..._args: unknown[]) {} }),
}));
beforeEach(() => vi.stubEnv("OPENROUTER_API_KEY", ""));
afterEach(() => { vi.unstubAllEnvs(); vi.clearAllMocks(); });

it("selects an independent image model and prefers the environment API key", async () => {
    vi.stubEnv("ENTISIUM_IMAGE_MODEL", "custom-image-model");
    vi.stubEnv("OPENAI_API_KEY", "environment-test-key");
    const read = vi.fn();
    createHostImageGeneration({} as HostProjectService, { read } as unknown as CredentialStore);
    const options = vi.mocked(ImageGenerationService).mock.calls[0][1];
    expect(options.model).toBe("custom-image-model");
    expect(await options.resolveApiKey()).toBe("environment-test-key");
    expect(read).not.toHaveBeenCalled();
});

it("uses saved OpenAI API keys and ignores OAuth credentials", async () => {
    vi.stubEnv("OPENAI_API_KEY", "");
    vi.stubEnv("ENTISIUM_IMAGE_MODEL", "");
    const read = vi.fn().mockResolvedValueOnce({ type: "api_key", key: "saved-test-key" })
        .mockResolvedValueOnce({ type: "oauth", key: "oauth-token" });
    createHostImageGeneration({} as HostProjectService, { read } as unknown as CredentialStore);
    const options = vi.mocked(ImageGenerationService).mock.calls[0][1];
    const signal = new AbortController().signal;
    expect(options.model).toBe("gpt-image-2");
    expect(await options.resolveApiKey(signal)).toBe("saved-test-key");
    expect(read).toHaveBeenCalledWith("openai", { signal });
    expect(await options.resolveApiKey()).toBeUndefined();
});

it("uses the selected provider URL and credentials rather than the OpenAI environment key", async () => {
    vi.stubEnv("OPENAI_API_KEY", "unrelated-openai-key");
    const config = parseConfig({ version: 1, providers: {
        images: { type: "openai-compatible", images: { baseUrl: "https://images.example.com/api/v1" }, apiKey: "image-test-key" },
    }, imageGeneration: { model: { provider: "images", id: "test-image" } } });
    const read = vi.fn(async () => ({ type: "api_key", key: "image-test-key" }));
    createHostImageGeneration({} as HostProjectService, { read } as unknown as CredentialStore, config);
    const options = vi.mocked(ImageGenerationService).mock.calls[0][1];
    expect(options.baseUrl).toBe("https://images.example.com/api/v1");
    expect(await options.resolveApiKey()).toBe("image-test-key");
    expect(read).toHaveBeenCalledWith("images", { signal: undefined });
});

it("requires a custom provider URL instead of silently using the official endpoint", () => {
    const config = parseConfig({ version: 1, providers: { images: { type: "openai-compatible", apiKey: "image-test-key" } }, imageGeneration: { model: { provider: "images", id: "test-image" } } });
    expect(() => createHostImageGeneration({} as HostProjectService, {} as CredentialStore, config)).toThrow("requires baseUrl");
});

it("passes the OpenRouter image protocol independently of the conversation protocol", async () => {
    const config = parseConfig({ version: 1, providers: { router: {
        type: "openrouter", chat: { api: "responses", baseUrl: "https://chat.example.com/v1" }, apiKey: "router-key",
    } }, imageGeneration: { model: { provider: "router", id: "openai/gpt-image-2" } } });
    const read = vi.fn(async () => ({ type: "api_key", key: "router-key" }));
    createHostImageGeneration({} as HostProjectService, { read } as unknown as CredentialStore, config);
    const options = vi.mocked(ImageGenerationService).mock.calls[0][1];
    expect(options).toMatchObject({ api: "openrouter-images", baseUrl: "https://openrouter.ai/api/v1" });
    expect(await options.resolveApiKey()).toBe("router-key");
    expect(read).toHaveBeenCalledWith("router", { signal: undefined });
});

it("uses FAL_KEY and the official fal.ai protocol without requiring a base URL", async () => {
    vi.stubEnv("FAL_KEY", "fal-environment-key");
    vi.stubEnv("OPENAI_API_KEY", "unrelated-openai-key");
    const config = parseConfig({ version: 1, providers: { fal: { type: "fal", apiKey: "saved-fal-key" } }, imageGeneration: {
        model: { provider: "fal", id: "openai/gpt-image-2.5/sunburst/text-to-image" },
    } });
    const read = vi.fn();
    createHostImageGeneration({} as HostProjectService, { read } as unknown as CredentialStore, config);
    const options = vi.mocked(ImageGenerationService).mock.calls[0][1];
    expect(options).toMatchObject({ api: "fal-images", model: "openai/gpt-image-2.5/sunburst/text-to-image" });
    expect(options.baseUrl).toBeUndefined();
    expect(await options.resolveApiKey()).toBe("fal-environment-key");
    expect(read).not.toHaveBeenCalled();
});
