import { randomUUID } from "node:crypto";
import { setTimeout as delay } from "node:timers/promises";
import type { SpriteAnimationResult } from "../contracts/sprite-animation.js";
import { spriteVideoJobSchema, type SpriteVideoInput, type SpriteVideoOptions } from "../contracts/sprite-video.js";
import { HostProjectService } from "../workspace/project-service.js";
import { decodePng } from "./processing.js";
import { videoQueueSchema, videoPrompt, VideoProviderFailure, type SpriteVideoProvider } from "./video-provider.js";
import { checkVideoTools, processSpriteVideo } from "./video-processing.js";

const json = (value: unknown) => Buffer.from(JSON.stringify(value, null, 2) + "\n");
export class SpriteVideoService {
    constructor(private readonly project: HostProjectService, private readonly provider?: SpriteVideoProvider,
        private readonly processVideo = processSpriteVideo, private readonly checkTools = checkVideoTools) {}

    async invoke(input: SpriteVideoInput, signal?: AbortSignal): Promise<SpriteAnimationResult> {
        signal?.throwIfAborted();
        if (input.operation === "reprocess") {
            await this.checkTools(signal);
            const path = input.video ?? `${input.run}/video.mp4`;
            const video = await this.project.read(path);
            if (!video) throw new Error("Source video does not exist.");
            return this.compose(input, video, input.name, input, signal);
        }
        const jobPath = `${input.run}/video-job.json`;
        let saved = await this.project.read(jobPath);
        if (input.operation === "create") {
            const job = spriteVideoJobSchema.parse({ characterImage: input.characterImage, action: input.action, name: input.name,
                model: input.model, seed: input.seed, cellSize: input.cellSize, frameCount: input.frameCount, align: input.align });
            if (saved) {
                if (JSON.stringify(spriteVideoJobSchema.parse(JSON.parse(saved.toString()))) !== JSON.stringify(job)) {
                    throw new Error("This run already belongs to a different request. Query its status or choose a new run.");
                }
            } else {
                if (!this.provider) throw new Error("Video generation is not configured.");
                await this.checkTools(signal);
                const reference = await this.project.read(job.characterImage);
                if (!reference || reference.length > 10 * 1024 * 1024) throw new Error("Character PNG is missing or exceeds 10 MiB.");
                await decodePng(reference);
                signal?.throwIfAborted();
                // Exclusive reservation prevents simultaneous hosts from making duplicate paid requests.
                await this.project.writeNew(jobPath, json(job));
                saved = json(job);
                await this.project.writeNew(`${input.run}/video-reference.png`, reference);
                await this.project.writeNew(`${input.run}/video-attempt.json`, json({ model: job.model, prompt: videoPrompt(job), seed: job.seed,
                    duration: 5, resolution: "480P", promptExpansionMode: "balanced", createdAt: new Date().toISOString() }));
                try {
                    const queue = await this.provider.submit(job, reference, signal);
                    // Persist acknowledgement even if the caller disconnected while submission completed.
                    await this.project.writeNew(`${input.run}/video-queue.json`, json(queue));
                } catch {
                    return this.result(input, "uncertain", [], "Submission outcome is uncertain. It was not retried. Check the provider before starting a new run.");
                }
            }
        }
        if (!saved) throw new Error("Unknown video job. Use create first, or reprocess with an existing video.");
        const job = spriteVideoJobSchema.parse(JSON.parse(saved.toString()));
        const completed = await this.project.read(`${input.run}/video-completed.json`);
        if (completed) return { ...JSON.parse(completed.toString()) as SpriteAnimationResult, operation: input.operation };
        let video = await this.project.read(`${input.run}/video.mp4`);
        if (!video) {
            const record = await this.project.read(`${input.run}/video-queue.json`);
            if (!record) return this.result(input, "uncertain", [], "No queue acknowledgement was saved. Do not resubmit automatically; check the provider for this run.");
            if (!this.provider) throw new Error("Video provider is not configured.");
            const queue = videoQueueSchema.parse(JSON.parse(record.toString()));
            const deadline = Date.now() + input.waitMs;
            try {
                while (!(await this.provider.status(queue, signal))) {
                    if (Date.now() >= deadline) return this.result(input, "pending", [], "Call status with the same run to resume. No additional generation is needed.");
                    await delay(Math.min(3000, Math.max(1, deadline - Date.now())), undefined, { signal });
                }
                const result = await this.provider.result(queue, signal);
                await this.project.writeNew(`${input.run}/video.mp4`, result.video);
                video = result.video;
                await this.project.write(`${input.run}/video-result.json`, json({ expandedPrompt: result.expandedPrompt }));
            } catch (error) {
                signal?.throwIfAborted();
                if (error instanceof VideoProviderFailure) return this.result(input, "failed", [], error.message);
                return this.result(input, "pending", [], "Could not retrieve the existing video task. Query status again; it will not submit another generation.");
            }
        }
        await this.checkTools(signal);
        try {
            const result = await this.compose(input, video, job.name, job, signal);
            await this.project.writeNew(`${input.run}/video-completed.json`, json(result));
            return result;
        } catch (error) {
            signal?.throwIfAborted();
            return this.result(input, "failed", [`${input.run}/video.mp4`], `Local processing failed: ${error instanceof Error ? error.message : "unknown error"}. The video is saved; status retries local processing only.`);
        }
    }

    private result(input: SpriteVideoInput, status: NonNullable<SpriteAnimationResult["status"]>, paths: string[], message?: string): SpriteAnimationResult {
        return { operation: input.operation, run: input.run, jobId: input.run, status, paths, message };
    }

    private async compose(input: SpriteVideoInput, video: Buffer, name: string, options: SpriteVideoOptions, signal?: AbortSignal) {
        const result = await this.processVideo(video, options, signal), cell = options.cellSize;
        const output = `${input.run}/exports/${randomUUID()}`;
        const manifest = { schema: "entisium.sprite-animation", version: 1, image: "atlas.png", width: cell * result.frames.length, height: cell,
            origin: "top-left", pivot: { x: 0.5, y: 1 - Math.max(1, Math.round(cell * 0.05)) / cell },
            animations: [{ name, loop: true, fps: result.frames.length / result.processing.cycleSeconds,
                frames: result.selected.map((sourceIndex, i) => ({ x: i * cell, y: 0, width: cell, height: cell, sourceIndex, durationMs: result.durations[i] })) }],
            processing: result.processing, quality: "quality.json", requiresVisualReview: true, warnings: result.quality.warnings };
        await this.project.writeNew(`${output}/atlas.png`, result.atlas);
        await this.project.writeNew(`${output}/quality.json`, json(result.quality));
        for (let i = 0; i < result.frames.length; i++) {
            signal?.throwIfAborted();
            await this.project.writeNew(`${output}/frames/${i}.png`, result.frames[i]);
        }
        const data = JSON.stringify(manifest).replaceAll("<", "\\u003c");
        await this.project.writeNew(`${output}/preview.html`, Buffer.from(`<!doctype html><meta charset="utf-8"><title>Sprite preview</title>
<style>body{background:#252830;color:white;font:16px system-ui;padding:32px}canvas{background:repeating-conic-gradient(#ccc 0% 25%,#eee 0% 50%) 0/20px 20px}button{margin:16px}a{color:skyblue}</style>
<h1>Sprite preview</h1><p>Review edges, limb motion and the loop seam. <a href="animation.json">Animation JSON</a> · <a href="quality.json">Quality</a></p><canvas></canvas><button>Pause</button><span></span>
<script>const data=${data},frames=data.animations[0].frames,total=frames.reduce((sum,f)=>sum+f.durationMs,0),canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d'),image=new Image();canvas.width=data.height;canvas.height=data.height;let time=0,last,paused=false;document.querySelector('button').onclick=()=>{paused=!paused;document.querySelector('button').textContent=paused?'Play':'Pause'};image.onload=()=>requestAnimationFrame(function tick(now){if(last!==undefined&&!paused)time+=now-last;last=now;let t=time%total,i=0;while(i<frames.length-1&&t>=frames[i].durationMs)t-=frames[i++].durationMs;const f=frames[i];ctx.clearRect(0,0,f.width,f.height);ctx.drawImage(image,f.x,f.y,f.width,f.height,0,0,f.width,f.height);document.querySelector('span').textContent=(i+1)+' / '+frames.length;requestAnimationFrame(tick)});image.onerror=()=>document.querySelector('span').textContent='Could not load atlas.png';image.src='atlas.png';</script>`));
        signal?.throwIfAborted();
        await this.project.writeNew(`${output}/animation.json`, json(manifest));
        return { ...this.result(input, "completed", [`${output}/animation.json`, `${output}/atlas.png`, `${output}/preview.html`, `${output}/quality.json`]), warnings: result.quality.warnings };
    }
}
