import { expect, it } from "vitest";
import { chatConnection, imageConnection } from "../src/settings/providers.js";
import { parseConfig } from "../src/settings/config.js";

it("resolves platform defaults independently for chat and images", () => {
    const settings = { type: "openrouter" as const, chat: { api: "responses" as const, baseUrl: "https://chat.example/v1" } };
    expect(chatConnection("router", settings)).toEqual({ api: "responses", baseUrl: "https://chat.example/v1" });
    expect(imageConnection("router", settings)).toEqual({ api: "openrouter-images", baseUrl: "https://openrouter.ai/api/v1" });
    expect(chatConnection("fal", { type: "fal" })).toBeUndefined();
    expect(imageConnection("fal", { type: "fal" })).toEqual({ api: "fal-images", baseUrl: undefined });
});

it("supports image-only custom connections without assuming a chat API", () => {
    const settings = { type: "openai-compatible" as const, images: { baseUrl: "https://images.example/v1" } };
    expect(chatConnection("custom", settings)).toBeUndefined();
    expect(imageConnection("custom", settings)).toEqual({ api: "openai-images", baseUrl: "https://images.example/v1" });
    expect(() => imageConnection("custom", { type: "openai-compatible", chat: { baseUrl: "https://chat.example/v1" } })).toThrow("requires baseUrl");
    expect(() => imageConnection("fal", { type: "fal", images: { baseUrl: "https://custom.example" } })).toThrow("official endpoints");
});

it("requires explicit valid connection schemas and model references", () => {
    for (const providers of [
        { router: { type: "unknown" } },
        { router: { type: "openrouter", chat: { api: "fal-images" } } },
        { router: { type: "openrouter", images: { api: "responses" } } },
        { router: { type: "openrouter", images: { baseUrl: "https://secret@example.com" } } },
    ]) expect(() => parseConfig({ version: 1, providers })).toThrow("Invalid config.yaml fields");
    expect(() => parseConfig({ version: 1, imageGeneration: { model: "old-shape" } })).toThrow("imageGeneration.model");
    expect(() => parseConfig({ version: 1, providers: { custom: { baseUrl: "https://example.com", api: "responses" } } })).toThrow();
});
