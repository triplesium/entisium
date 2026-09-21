import { z } from "zod/v4";
import type { SpriteVideoJob } from "../contracts/sprite-video.js";

export const videoQueueSchema = z.object({ request_id: z.string().min(1), status_url: z.string().url(), response_url: z.string().url() });
export type VideoQueue = z.infer<typeof videoQueueSchema>;
export class VideoProviderFailure extends Error {}
export interface SpriteVideoProvider {
    submit(job: SpriteVideoJob, reference: Buffer, signal?: AbortSignal): Promise<VideoQueue>;
    status(queue: VideoQueue, signal?: AbortSignal): Promise<boolean>;
    result(queue: VideoQueue, signal?: AbortSignal): Promise<{ video: Buffer; expandedPrompt?: string | null }>;
}

export function videoPrompt(job: SpriteVideoJob) {
    return `Animate the reference character: ${job.action}. Produce repeated complete action cycles in place. Preserve character identity, face, clothing and proportions. Fixed camera and side view, no turning or horizontal travel. Full body visible with margins throughout. Keep a perfectly uniform bright green (#00ff00) background, including gaps between limbs, with no shadows, scenery or text. Smooth cartoon motion with natural limb alternation. No audio is needed.`;
}

/** Credentials stay in the host; no API keys or data URIs are persisted in job records. */
export class FalSpriteVideoProvider implements SpriteVideoProvider {
    constructor(private readonly credential: () => Promise<string | undefined>, private readonly fetcher: typeof fetch = fetch) {}

    private async request(url: string, signal?: AbortSignal, body?: unknown) {
        const target = new URL(url);
        if (target.origin !== "https://queue.fal.run" || target.username || target.password) throw new Error("Invalid video queue URL.");
        const key = await this.credential();
        if (!key) throw new Error("Configure the fal API key to generate sprite videos.");
        const response = await this.fetcher(url, { method: body ? "POST" : "GET", redirect: "error",
            headers: { Authorization: `Key ${key}`, "Content-Type": "application/json" },
            body: body ? JSON.stringify(body) : undefined,
            signal: signal ? AbortSignal.any([signal, AbortSignal.timeout(30000)]) : AbortSignal.timeout(30000) });
        if (!response.ok) {
            if (response.status >= 400 && response.status < 500 && ![408, 429].includes(response.status)) {
                throw new VideoProviderFailure(`Video provider HTTP ${response.status}. Check provider task details or credentials; no new generation was submitted.`);
            }
            throw new Error(`Video provider HTTP ${response.status}.`);
        }
        return response.json();
    }

    async submit(job: SpriteVideoJob, reference: Buffer, signal?: AbortSignal) {
        return videoQueueSchema.parse(await this.request(`https://queue.fal.run/${job.model}`, signal, {
            prompt: videoPrompt(job), image_url: `data:image/png;base64,${reference.toString("base64")}`,
            seed: job.seed, duration: 5, resolution: "480P", prompt_expansion_mode: "balanced", enable_safety_checker: true,
        }));
    }

    async status(queue: VideoQueue, signal?: AbortSignal) {
        const value = z.object({ status: z.enum(["IN_QUEUE", "IN_PROGRESS", "COMPLETED"]) }).parse(await this.request(queue.status_url, signal));
        return value.status === "COMPLETED";
    }

    async result(queue: VideoQueue, signal?: AbortSignal) {
        const result = z.object({ video: z.object({ url: z.string().url() }), expanded_prompt: z.string().nullable().optional() })
            .parse(await this.request(queue.response_url, signal));
        const url = new URL(result.video.url);
        if (url.protocol !== "https:" || !url.hostname.endsWith(".fal.media") || url.username || url.password) throw new Error("Invalid video download URL.");
        const response = await this.fetcher(url, { redirect: "error", signal: signal ? AbortSignal.any([signal, AbortSignal.timeout(60000)]) : AbortSignal.timeout(60000) });
        if (!response.ok || !response.body) throw new Error("Could not download sprite video.");
        const reader = response.body.getReader(), chunks: Uint8Array[] = [];
        let size = 0;
        try {
            while (true) {
                const chunk = await reader.read(); if (chunk.done) break;
                size += chunk.value.length;
                if (size > 100 * 1024 * 1024) throw new Error("Video exceeds 100 MiB.");
                chunks.push(chunk.value);
            }
        } finally { await reader.cancel(); }
        const video = Buffer.concat(chunks);
        if (video.subarray(4, 8).toString() !== "ftyp") throw new Error("Provider did not return an MP4 video.");
        return { video, expandedPrompt: result.expanded_prompt };
    }
}
