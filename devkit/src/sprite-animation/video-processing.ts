import { execFile } from "node:child_process";
import { mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";
import sharp from "sharp";
import type { SpriteVideoOptions } from "../contracts/sprite-video.js";
import { bounds, register } from "./refinement.js";
import { matteGreen } from "./video-matte.js";
import { inspectFrames } from "./quality.js";

const execute = promisify(execFile);
export async function checkVideoTools(signal?: AbortSignal) {
    for (const executable of ["ffmpeg", "ffprobe"]) {
        try { await execute(executable, ["-version"], { signal, timeout: 10000, maxBuffer: 1024 * 1024, windowsHide: true }); }
        catch { signal?.throwIfAborted(); throw new Error("Sprite video processing requires ffmpeg and ffprobe on PATH."); }
    }
}

export type SpriteVideoExport = Awaited<ReturnType<typeof processSpriteVideo>>;
export async function processSpriteVideo(video: Buffer, options: SpriteVideoOptions, signal?: AbortSignal) {
    if (video.length > 100 * 1024 * 1024 || video.subarray(4, 8).toString() !== "ftyp") throw new Error("Expected an MP4 smaller than 100 MiB.");
    const temp = await mkdtemp(join(tmpdir(), "entisium-sprite-"));
    try {
        const source = join(temp, "source.mp4"); await writeFile(source, video);
        const run = (tool: string, args: string[]) => execute(tool, args, { signal, timeout: 120000, maxBuffer: 4 * 1024 * 1024, windowsHide: true });
        // Only local file and pipe protocols; external references in imported media cannot fetch URLs.
        const probe = JSON.parse((await run("ffprobe", ["-v", "error", "-protocol_whitelist", "file,pipe", "-select_streams", "v:0", "-show_entries", "stream=width,height,nb_frames,duration", "-of", "json", source])).stdout);
        const stream = probe.streams?.[0], width = Number(stream?.width), height = Number(stream?.height);
        const count = Number(stream?.nb_frames), duration = Number(stream?.duration);
        if (!Number.isInteger(width) || !Number.isInteger(height) || width < 16 || height < 16 || width > 2048 || height > 2048 ||
            !Number.isInteger(count) || count < 8 || count > 360 || !Number.isFinite(duration) || duration <= 0 || duration > 15 || width * height * count > 64 * 1024 * 1024) {
            throw new Error("Video must be 8–360 frames, at most 15 seconds, 2048 pixels per side and 64 megapixels across all frames.");
        }
        const timestamps = JSON.parse((await run("ffprobe", ["-v", "error", "-protocol_whitelist", "file,pipe", "-select_streams", "v:0", "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", source])).stdout)
            .frames.map((frame: { best_effort_timestamp_time: string }) => Number(frame.best_effort_timestamp_time)) as number[];
        if (timestamps.length !== count || timestamps.some((time, i) => !Number.isFinite(time) || (i > 0 && time <= timestamps[i - 1]))) throw new Error("Video timestamps are missing or invalid.");
        await run("ffmpeg", ["-v", "error", "-n", "-protocol_whitelist", "file,pipe", "-i", source, "-map", "0:v:0", "-frames:v", "361", "-fps_mode", "passthrough", join(temp, "%04d.png")]);
        const names = (await readdir(temp)).filter(name => /^\d+\.png$/.test(name)).sort();
        if (names.length !== count) throw new Error("Decoded frame count differs from video metadata.");
        const cells: Buffer[] = [];
        for (const name of names) {
            signal?.throwIfAborted();
            const data = await sharp(await readFile(join(temp, name))).ensureAlpha().raw().toBuffer();
            const pixels = matteGreen(data, width, height), box = bounds(pixels, width, height);
            if (box.right < box.left || box.left === 0 || box.top === 0 || box.right === width - 1 || box.bottom === height - 1) {
                throw new Error("Empty or clipped character after matting. Use a flat green background and keep the whole character inside the frame.");
            }
            cells.push(pixels);
        }
        const registration = register(cells, width, height, options.align);
        const thumbnails = await Promise.all(cells.map((pixels, i) => {
            const shifted = shiftFrame(pixels, width, height, registration.offsets[i].dx);
            return sharp(shifted, { raw: { width, height, channels: 4 } }).resize(32, 32).raw().toBuffer();
        }));
        const { start, period, profile } = selectCycle(thumbnails, timestamps);
        const selected = Array.from({ length: Math.min(options.frameCount, period) }, (_, i) => start + Math.floor(i * period / Math.min(options.frameCount, period)));
        let left = width, top = height, right = -1, bottom = -1;
        for (const index of selected) {
            const box = bounds(cells[index], width, height), dx = registration.offsets[index].dx;
            left = Math.min(left, box.left + dx); right = Math.max(right, box.right + dx);
            top = Math.min(top, box.top); bottom = Math.max(bottom, box.bottom);
        }
        const crop = { left, top, width: right - left + 1, height: bottom - top + 1 };
        const cell = options.cellSize, padding = Math.max(1, Math.round(cell * 0.05));
        const scale = Math.min((cell - 2 * padding) / crop.width, (cell - 2 * padding) / crop.height);
        const outputWidth = Math.max(1, Math.round(crop.width * scale)), outputHeight = Math.max(1, Math.round(crop.height * scale));
        const frames: Buffer[] = [];
        for (const index of selected) {
            signal?.throwIfAborted();
            const pixels = cells[index], dx = registration.offsets[index].dx, canvas = Buffer.alloc(crop.width * crop.height * 4);
            for (let y = top; y <= bottom; y++) for (let x = left; x <= right; x++) {
                const sx = x - dx;
                if (sx >= 0 && sx < width) pixels.copy(canvas, ((y - top) * crop.width + x - left) * 4, (y * width + sx) * 4, (y * width + sx) * 4 + 4);
            }
            const content = await sharp(canvas, { raw: { width: crop.width, height: crop.height, channels: 4 } })
                .resize(outputWidth, outputHeight, { kernel: "lanczos3" }).png().toBuffer();
            frames.push(await sharp({ create: { width: cell, height: cell, channels: 4, background: "#00000000" } })
                .composite([{ input: content, left: Math.floor((cell - outputWidth) / 2), top: cell - padding - outputHeight }]).png().toBuffer());
        }
        const atlas = await sharp({ create: { width: frames.length * cell, height: cell, channels: 4, background: "#00000000" } })
            .composite(frames.map((input, i) => ({ input, left: i * cell, top: 0 }))).png().toBuffer();
        const durations = selected.map((index, i) => 1000 * (timestamps[i + 1 < selected.length ? selected[i + 1] : start + period] - timestamps[index]));
        const quality = await inspectFrames(frames, "#00ff00", true);
        return { atlas, frames, selected, durations, quality,
            processing: { sourceSize: { width, height }, sourceFrameCount: count, crop, registration, start, period, profile, cycleSeconds: timestamps[start + period] - timestamps[start], method: "native-resolution-soft-green-matte-lanczos3" } };
    } finally { await rm(temp, { recursive: true, force: true }); }
}

function shiftFrame(pixels: Buffer, width: number, height: number, dx: number) {
    const output = Buffer.alloc(pixels.length), start = Math.max(0, -dx), end = Math.min(width, width - dx);
    for (let y = 0; y < height; y++) pixels.copy(output, (y * width + start + dx) * 4, (y * width + start) * 4, (y * width + end) * 4);
    return output;
}

export function selectCycle(frames: Buffer[], times: number[]) {
    const distance = (a: Buffer, b: Buffer) => {
        let total = 0, weight = 0;
        for (let p = 0; p < a.length; p += 4) {
            const aa = a[p + 3] / 255, ba = b[p + 3] / 255;
            weight += Math.max(aa, ba); total += Math.abs(aa - ba);
            for (let c = 0; c < 3; c++) total += Math.abs(a[p + c] * aa - b[p + c] * ba) / (255 * 3);
        }
        return total / Math.max(1, weight * 2);
    };
    const matrix = frames.map(a => frames.map(b => distance(a, b)));
    const step = (times.at(-1)! - times[0]) / (times.length - 1);
    const min = Math.max(2, Math.ceil(0.5 / step)), max = Math.min(frames.length - 2, Math.floor(2.5 / step));
    const profile = Array.from({ length: Math.max(0, max - min + 1) }, (_, i) => {
        const lag = min + i;
        return { lag, difference: matrix.slice(0, -lag).reduce((sum, row, index) => sum + row[index + lag], 0) / (frames.length - lag) };
    });
    const minima = profile.filter((entry, i) => i > 0 && i < profile.length - 1 && entry.difference < profile[i - 1].difference && entry.difference <= profile[i + 1].difference);
    if (!minima.length) throw new Error("No repeatable cycle found. Existing video is preserved; inspect it before generating another take.");
    const deepest = Math.min(...minima.map(entry => entry.difference));
    const period = minima.find(entry => entry.difference <= deepest * 1.15)!.lag;
    const candidates = Array.from({ length: frames.length - period }, (_, start) => {
        let sum = 0;
        for (let i = 0; i < period - 1; i++) sum += matrix[start + i][start + i + 1];
        return { start, ratio: matrix[start][start + period] / Math.max(0.01, sum / (period - 1)) };
    }).sort((a, b) => a.ratio - b.ratio);
    return { start: candidates[0].start, period, profile };
}
