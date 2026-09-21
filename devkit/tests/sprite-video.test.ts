import { afterEach, expect, it, vi } from "vitest";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import sharp from "sharp";
import { HostProjectService } from "../src/workspace/project-service.js";
import { spriteVideoCreateSchema, spriteVideoStatusSchema, spriteVideoReprocessSchema } from "../src/contracts/sprite-video.js";
import { SpriteVideoService } from "../src/sprite-animation/video-service.js";
import { FalSpriteVideoProvider, VideoProviderFailure } from "../src/sprite-animation/video-provider.js";
import { matteGreen } from "../src/sprite-animation/video-matte.js";
import { selectCycle } from "../src/sprite-animation/video-processing.js";
import { inspectFrames } from "../src/sprite-animation/quality.js";

const cleanup: (() => Promise<void>)[] = [];
afterEach(async () => { for (const dispose of cleanup.splice(0)) await dispose(); });
const input = spriteVideoCreateSchema.parse({ operation: "create", run: "assets/walk", characterImage: "assets/hero.png", action: "Walk in place", waitMs: 0 });
const status = spriteVideoStatusSchema.parse({ operation: "status", run: input.run, waitMs: 0 });
const queue = { request_id: "test", status_url: "https://queue.fal.run/test/status", response_url: "https://queue.fal.run/test/result" };

async function fixture() {
    const root = await mkdtemp(join(tmpdir(), "sprite-video-test-"));
    await writeFile(join(root, "project.yaml"), "name: Test\n");
    const project = new HostProjectService(root);
    cleanup.push(async () => { project.dispose(); await rm(root, { recursive: true, force: true }); });
    const frame = await sharp({ create: { width: 256, height: 256, channels: 4, background: "red" } }).png().toBuffer();
    await project.writeNew(input.characterImage, frame);
    const provider = { submit: vi.fn(async () => queue), status: vi.fn(async () => true), result: vi.fn(async () => ({ video: Buffer.from("0000ftypvideo"), expandedPrompt: "Walk" })) };
    const processing = vi.fn(async () => ({ atlas: frame, frames: [frame], selected: [3], durations: [1250], quality: await inspectFrames([frame], "#00ff00", true),
        processing: { sourceSize: { width: 480, height: 480 }, sourceFrameCount: 124, crop: { left: 10, top: 10, width: 200, height: 400 },
            registration: { referenceIndex: 0, offsets: [] }, start: 3, period: 30, profile: [], cycleSeconds: 1.25, method: "test" } }));
    const check = vi.fn(async () => {});
    const service = () => new SpriteVideoService(project, provider, processing, check);
    return { project, provider, processing, check, service };
}

it("creates and exports in one call, then reuses the completed job after restart", async () => {
    const f = await fixture();
    const result = await f.service().invoke(input);
    expect(result.status).toBe("completed");
    const manifest = JSON.parse((await f.project.read(result.paths[0]))!.toString());
    expect(manifest.animations[0].frames[0]).toMatchObject({ sourceIndex: 3, durationMs: 1250 });
    expect(await f.service().invoke(input)).toEqual(result);
    expect((await f.service().invoke(status)).paths).toEqual(result.paths);
    expect(f.provider.submit).toHaveBeenCalledOnce(); expect(f.processing).toHaveBeenCalledOnce();
});

it("persists queued work and resumes without resubmission", async () => {
    const f = await fixture(); f.provider.status.mockResolvedValueOnce(false);
    expect((await f.service().invoke(input)).status).toBe("pending");
    expect((await f.service().invoke(status)).status).toBe("completed");
    expect(f.provider.submit).toHaveBeenCalledOnce();
});

it("never retries uncertain submissions, including repeated create", async () => {
    const f = await fixture(); f.provider.submit.mockRejectedValue(new Error("connection lost"));
    expect((await f.service().invoke(input)).status).toBe("uncertain");
    expect((await f.service().invoke(input)).status).toBe("uncertain");
    expect((await f.service().invoke(status)).status).toBe("uncertain");
    expect(f.provider.submit).toHaveBeenCalledOnce();
});

it("refuses conflicting requests and reserves a run across simultaneous services", async () => {
    const f = await fixture();
    await Promise.allSettled([f.service().invoke(input), f.service().invoke(input)]);
    expect(f.provider.submit).toHaveBeenCalledOnce();
    await expect(f.service().invoke({ ...input, action: "Run" })).rejects.toThrow("different request");
});

it("retries local processing using the saved video and reprocesses without provider calls", async () => {
    const f = await fixture(); f.processing.mockRejectedValueOnce(new Error("no cycle"));
    expect((await f.service().invoke(input)).status).toBe("failed");
    expect((await f.service().invoke(status)).status).toBe("completed");
    const before = f.provider.result.mock.calls.length;
    const output = await f.service().invoke(spriteVideoReprocessSchema.parse({ operation: "reprocess", run: input.run }));
    expect(output.status).toBe("completed"); expect(f.provider.result).toHaveBeenCalledTimes(before);
    expect(f.provider.submit).toHaveBeenCalledOnce();
});

it("supports local import without a provider and rejects path traversal", async () => {
    const f = await fixture(); await f.project.writeNew("assets/import.mp4", Buffer.from("0000ftypvideo"));
    const result = await new SpriteVideoService(f.project, undefined, f.processing, f.check).invoke(
        spriteVideoReprocessSchema.parse({ operation: "reprocess", run: "assets/import-run", video: "assets/import.mp4" }));
    expect(result.status).toBe("completed"); expect(f.provider.submit).not.toHaveBeenCalled();
    expect(() => spriteVideoReprocessSchema.parse({ operation: "reprocess", run: "assets/../outside", video: "https://example.com/a.mp4" })).toThrow();
});

it("validates local dependencies before reserving or submitting paid work", async () => {
    const f = await fixture(); f.check.mockRejectedValue(new Error("missing ffmpeg"));
    await expect(f.service().invoke(input)).rejects.toThrow("ffmpeg");
    expect(f.provider.submit).not.toHaveBeenCalled(); expect(await f.project.read(`${input.run}/video-job.json`)).toBeUndefined();
});

it("distinguishes provider rejection from a retryable status transport error", async () => {
    const f = await fixture(); f.provider.status.mockRejectedValueOnce(new VideoProviderFailure("rejected"));
    expect((await f.service().invoke(input)).status).toBe("failed");
    f.provider.status.mockRejectedValueOnce(new Error("offline"));
    expect((await f.service().invoke(status)).status).toBe("pending");
    expect(f.provider.submit).toHaveBeenCalledOnce();
});

it("does not leak fal credentials to queue URLs outside fal", async () => {
    const fetcher = vi.fn(); const credential = vi.fn(async () => "secret");
    const provider = new FalSpriteVideoProvider(credential, fetcher);
    await expect(provider.status({ ...queue, status_url: "https://example.com/steal" })).rejects.toThrow("Invalid");
    expect(fetcher).not.toHaveBeenCalled(); expect(credential).not.toHaveBeenCalled();
});

it("submits the configured H3 shape and retrieves media without forwarding credentials", async () => {
    const fetcher = vi.fn()
        .mockResolvedValueOnce(Response.json(queue))
        .mockResolvedValueOnce(Response.json({ status: "COMPLETED" }))
        .mockResolvedValueOnce(Response.json({ video: { url: "https://v3b.fal.media/video.mp4" }, expanded_prompt: "expanded" }))
        .mockResolvedValueOnce(new Response(Buffer.from("0000ftypvideo")));
    const provider = new FalSpriteVideoProvider(async () => "secret", fetcher);
    const accepted = await provider.submit(input, Buffer.from("png"));
    expect(JSON.parse(fetcher.mock.calls[0][1].body)).toMatchObject({ duration: 5, resolution: "480P", prompt_expansion_mode: "balanced", enable_safety_checker: true });
    expect(await provider.status(accepted)).toBe(true);
    expect((await provider.result(accepted)).expandedPrompt).toBe("expanded");
    expect(fetcher.mock.calls[3][1].headers).toBeUndefined();
    expect(fetcher.mock.calls[3][1].redirect).toBe("error");
});

it("rejects cancellation before making a reservation or paying", async () => {
    const f = await fixture();
    await expect(f.service().invoke(input, AbortSignal.abort())).rejects.toThrow();
    expect(f.provider.submit).not.toHaveBeenCalled();
    expect(await f.project.read(`${input.run}/video-job.json`)).toBeUndefined();
});

it("recovers soft grey boundary coverage without green spill and preserves opaque color", () => {
    const width = 32, pixels = Buffer.alloc(width * width * 4);
    for (let p = 0; p < pixels.length; p += 4) pixels.set([0, 255, 0, 255], p);
    for (let y = 8; y < 24; y++) for (let x = 8; x < 24; x++) pixels.set([80, 80, 80, 255], (y * width + x) * 4);
    for (let y = 8; y < 24; y++) pixels.set([40, 168, 40, 255], (y * width + 7) * 4);
    const result = matteGreen(pixels, width, width), edge = (16 * width + 7) * 4;
    expect(result[3]).toBe(0); expect([...result.subarray((16 * width + 16) * 4, (16 * width + 16) * 4 + 4)]).toEqual([80, 80, 80, 255]);
    expect(result[edge + 3]).toBeGreaterThan(110); expect(result[edge + 3]).toBeLessThan(145);
    expect(Math.max(...result.subarray(edge, edge + 3)) - Math.min(...result.subarray(edge, edge + 3))).toBeLessThan(3);
});

it("does not label a still clip as a detected motion loop", () => {
    const frame = Buffer.from([100, 0, 0, 255]);
    expect(() => selectCycle(Array(100).fill(frame), Array.from({ length: 100 }, (_, i) => i / 24))).toThrow("No repeatable cycle");
});
