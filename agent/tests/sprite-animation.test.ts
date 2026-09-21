import { expect, it, vi } from "vitest";
import { createSpriteAnimationTools } from "../src/tools/sprite-animation.js";

it("exposes one-call generation and forwards resumable job status", async () => {
    const invoke = vi.fn(async () => ({ operation: "create" as const, run: "assets/walk", paths: [], jobId: "assets/walk", status: "pending" as const }));
    const [tool] = createSpriteAnimationTools(invoke);
    const signal = new AbortController().signal;
    const result = await tool.execute("call", { operation: "create", run: "assets/walk", characterImage: "assets/hero.png", action: "Walk" }, signal);
    expect(result.details).toMatchObject({ status: "pending", jobId: "assets/walk" });
    expect(invoke).toHaveBeenLastCalledWith(expect.objectContaining({ operation: "create", cellSize: 256, frameCount: 16, waitMs: 20000 }), signal);
    await tool.execute("poll", { operation: "status", run: "assets/walk", waitMs: 0 }, signal);
    expect(invoke).toHaveBeenLastCalledWith({ operation: "status", run: "assets/walk", waitMs: 0 }, signal);
});

it("exposes a browser-safe staged tool, validates inputs and forwards cancellation", async () => {
    const invoke = vi.fn(async () => ({ operation: "compose" as const, run: "assets/run", paths: ["assets/run/exports/id/animation.json"] }));
    const [tool] = createSpriteAnimationTools(invoke);
    expect(tool.name).toBe("sprite_animation");
    const signal = new AbortController().signal;
    const result = await tool.execute("call", { operation: "compose", run: "assets/run" }, signal);
    expect(invoke).toHaveBeenCalledWith({ operation: "compose", run: "assets/run", tolerance: 40, despill: true, align: true, selections: [] }, signal);
    expect(result.details.paths).toHaveLength(1);
    await expect(tool.execute("call", { operation: "compose", run: "../bad" }, signal)).rejects.toThrow();
    await expect(tool.execute("call", { operation: "compose", run: "assets/run" }, AbortSignal.abort())).rejects.toThrow();
    expect(invoke).toHaveBeenCalledOnce();
});
