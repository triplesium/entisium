import { randomUUID } from "node:crypto";
import sharp from "sharp";
import { spriteAnimationSchema, spriteRequestSchema, type SpriteAnimationInput, type SpriteAnimationResult, type SpriteRequest } from "../contracts/sprite-animation.js";
import type { ImageGenerationInvoker } from "../contracts/image-generation.js";
import { HostProjectService } from "../workspace/project-service.js";
import { decodePng, extractRow, layoutGuide, rowDimensions } from "./processing.js";
import { inspectFrames } from "./quality.js";
import { SpriteVideoService } from "./video-service.js";
import type { SpriteVideoProvider } from "./video-provider.js";

/** Serialize a run within this process. Exclusive create also protects paid generation across hosts. */
const active = new Set<string>();
export class SpriteAnimationService {
    constructor(private readonly project: HostProjectService,
        private readonly images: (project: HostProjectService) => ImageGenerationInvoker,
        private readonly videos?: SpriteVideoProvider) {}

    async invoke(parameters: unknown, signal?: AbortSignal): Promise<SpriteAnimationResult> {
        const input = spriteAnimationSchema.parse(parameters);
        signal?.throwIfAborted();
        const root = await this.project.workspaceRoot();
        const key = `${root}/${input.run}`.toLowerCase();
        if (active.has(key)) throw new Error("This sprite run is busy.");
        active.add(key);
        const project = new HostProjectService(root);
        try {
            if (input.operation === "create" || input.operation === "status" || input.operation === "reprocess") {
                return await new SpriteVideoService(project, this.videos).invoke(input, signal);
            }
            if (input.operation === "prepare") return await this.prepare(project, input, signal);
            const source = await project.read(`${input.run}/request.json`);
            if (!source) throw new Error("Prepare the sprite run first.");
            const request = spriteRequestSchema.parse(JSON.parse(source.toString("utf8")));
            if (input.operation === "generate") return await this.generate(project, input, request, signal);
            return await this.compose(project, input, request, signal);
        } finally { project.dispose(); active.delete(key); }
    }

    private async prepare(project: HostProjectService, input: Extract<SpriteAnimationInput, { operation: "prepare" }>, signal?: AbortSignal) {
        const request = input.request;
        const reference = await project.read(request.reference);
        if (!reference) throw new Error("Character reference image does not exist.");
        await decodePng(reference);
        if (reference.length > 10 * 1024 * 1024) throw new Error("Character reference exceeds 10 MiB.");
        const motionReferences = new Map<string, Buffer>();
        for (const state of request.states) if (state.motionReference) {
            const motion = await project.read(state.motionReference);
            if (!motion) throw new Error(`Motion reference for ${state.name} does not exist.`);
            if (motion.length > 10 * 1024 * 1024) throw new Error("Motion reference exceeds 10 MiB.");
            await decodePng(motion);
            motionReferences.set(state.name, motion);
        }
        signal?.throwIfAborted();
        // Keep the original reference immutable even if its source asset is later edited.
        await project.writeNew(`${input.run}/base.png`, reference);
        const paths = [`${input.run}/base.png`];
        for (const state of request.states) {
            signal?.throwIfAborted();
            const motion = motionReferences.get(state.name);
            if (motion) {
                const path = `${input.run}/motion/${state.name}.png`;
                await project.writeNew(path, motion); paths.push(path);
            }
            const guide = `${input.run}/guides/${state.name}.png`, prompt = `${input.run}/prompts/${state.name}.txt`;
            await project.writeNew(guide, await layoutGuide(state.poses.length, request.chroma));
            await project.writeNew(prompt, Buffer.from(rowPrompt(request, state)));
            paths.push(guide, prompt);
        }
        signal?.throwIfAborted();
        const path = `${input.run}/request.json`;
        await project.writeNew(path, json(request));
        return { operation: input.operation, run: input.run, paths: [path, ...paths] };
    }

    private async generate(project: HostProjectService, input: Extract<SpriteAnimationInput, { operation: "generate" }>, request: SpriteRequest, signal?: AbortSignal) {
        const state = request.states.find(state => state.name === input.state);
        if (!state) throw new Error("Unknown sprite state.");
        const path = `${input.run}/raw/${state.name}.png`;
        if (await project.exists(path)) throw new Error("This state already has a raw image. Compose it or prepare a new run for another take.");
        const { width, height } = rowDimensions(state.poses.length);
        const dimensions = input.options?.size || input.options?.resolution || input.options?.aspect_ratio ? {} : { size: `${width}x${height}` };
        const invocation = {
            ...dimensions, ...input.options, background: "opaque" as const,
            prompt: rowPrompt(request, state), path,
            references: [`${input.run}/base.png`, `${input.run}/guides/${state.name}.png`],
        };
        if (state.motionReference) invocation.references.push(`${input.run}/motion/${state.name}.png`);
        signal?.throwIfAborted();
        // Persist an attempt before contacting a paid API. An uncertain outcome must never cause an automatic retry.
        await project.writeNew(`${input.run}/attempts/${state.name}.json`, json(invocation));
        try {
            await this.images(project)(invocation, signal);
        } catch {
            signal?.throwIfAborted();
            throw new Error(`Sprite generation failed for ${state.name}. The attempt was recorded and was not retried. Prepare a new run only after deciding to retry; existing raw images can still be composed.`);
        }
        return { operation: input.operation, run: input.run, paths: [path] };
    }

    private async compose(project: HostProjectService, input: Extract<SpriteAnimationInput, { operation: "compose" }>, request: SpriteRequest, signal?: AbortSignal) {
        const names = new Set(request.states.map(state => state.name));
        if (input.selections.some(selection => !names.has(selection.state)) ||
            new Set(input.selections.map(selection => selection.state)).size !== input.selections.length) throw new Error("Selections must name distinct states in the request.");
        const maxFrames = Math.max(...request.states.map(state => input.selections.find(selection => selection.state === state.name)?.order?.length ?? state.poses.length));
        if (maxFrames * request.states.length * request.cellSize ** 2 > 16 * 1024 * 1024) {
            throw new Error("Atlas exceeds 16 megapixels; reduce frame size or selections.");
        }
        const rows = [];
        const warnings: string[] = [];
        for (const state of request.states) {
            signal?.throwIfAborted();
            const selection = input.selections.find(selection => selection.state === state.name);
            const sourcePath = selection?.source ?? `${input.run}/raw/${state.name}.png`;
            const source = await project.read(sourcePath);
            if (!source) throw new Error(`Missing sprite row: ${sourcePath}`);
            const extracted = await extractRow(source, state.poses.length, request.cellSize, request.chroma, input.tolerance, input);
            const order = selection?.order ?? state.poses.map((_, index) => index);
            if (order.some(index => index >= extracted.frames.length)) throw new Error(`Frame selection is out of range for ${state.name}.`);
            const loop = selection?.loop ?? state.loop;
            const before = await inspectFrames(order.map(index => extracted.beforeFrames[index]), request.chroma, loop);
            const after = await inspectFrames(order.map(index => extracted.frames[index]), request.chroma, loop);
            warnings.push(...[...extracted.warnings, ...after.warnings].map(warning => `${state.name}: ${warning}`));
            rows.push({ state, source: sourcePath, extracted, order, before, after, fps: selection?.fps ?? state.fps, loop });
        }
        const cell = request.cellSize;
        const width = Math.max(...rows.map(row => row.order.length)) * cell, height = rows.length * cell;
        if (width * height > 16 * 1024 * 1024) throw new Error("Atlas exceeds 16 megapixels; reduce frame size or selections.");
        const layers = rows.flatMap((row, y) => row.order.map((index, x) => ({ input: row.extracted.frames[index], left: x * cell, top: y * cell })));
        const atlas = await sharp({ create: { width, height, channels: 4, background: "#00000000" } }).composite(layers).png().toBuffer();
        const beforeAtlas = await sharp({ create: { width, height, channels: 4, background: "#00000000" } })
            .composite(rows.flatMap((row, y) => row.order.map((index, x) => ({ input: row.extracted.beforeFrames[index], left: x * cell, top: y * cell })))).png().toBuffer();
        signal?.throwIfAborted();
        // Each compose is an immutable revision. The manifest is published last, after every referenced file.
        const output = `${input.run}/exports/${randomUUID()}`;
        const animations = rows.map((row, y) => ({
            name: row.state.name, fps: row.fps, loop: row.loop,
            frames: row.order.map((sourceIndex, x) => ({ x: x * cell, y: y * cell, width: cell, height: cell,
                durationMs: 1000 / row.fps, sourceIndex, label: row.state.poses[sourceIndex] })),
        }));
        const manifest = { schema: "entisium.sprite-animation", version: 1, image: "atlas.png", width, height,
            origin: "top-left", pivot: { x: 0.5, y: 1 - Math.max(1, Math.round(cell * 0.05)) / cell }, animations,
            quality: "quality.json", comparisonImage: "before-atlas.png",
            processing: { tolerance: input.tolerance, despill: input.despill, align: input.align, selections: input.selections,
                rows: rows.map(row => ({ name: row.state.name, source: row.source, sourceSize: row.extracted.sourceSize, crop: row.extracted.crop,
                    cleanup: row.extracted.cleanup, registration: row.extracted.registration })) }, warnings };
        await project.writeNew(`${output}/atlas.png`, atlas);
        await project.writeNew(`${output}/before-atlas.png`, beforeAtlas);
        await project.writeNew(`${output}/quality.json`, json({ version: 1, requiresVisualReview: true,
            note: "Heuristics do not verify anatomy or gait. Frame metrics follow selected playback order; registration offsets use raw source indices.",
            rows: rows.map(row => ({ name: row.state.name, order: row.order, before: row.before, after: row.after })) }));
        for (const row of rows) for (const [index, sourceIndex] of row.order.entries()) {
            signal?.throwIfAborted();
            await project.writeNew(`${output}/frames/${row.state.name}/${index}.png`, row.extracted.frames[sourceIndex]);
        }
        await project.writeNew(`${output}/preview.html`, Buffer.from(preview(manifest)));
        signal?.throwIfAborted();
        await project.writeNew(`${output}/animation.json`, json(manifest));
        return { operation: input.operation, run: input.run,
            paths: [`${output}/animation.json`, `${output}/atlas.png`, `${output}/preview.html`, `${output}/quality.json`, `${output}/before-atlas.png`], warnings };
    }
}

function rowPrompt(request: SpriteRequest, state: SpriteRequest["states"][number]) {
    return [
        `Create one horizontal animation row with exactly ${state.poses.length} equal-width cells, ordered left to right.`,
        `Reference 1 is the character identity. Preserve its face, colours, clothes, equipment, proportions and view. ${request.description}`,
        "Reference 2 only defines cell layout. Remove all guide lines; do not draw text, labels or borders.",
        ...(state.motionReference ? ["Reference 3 defines motion only: follow its pose phases, foot contacts and rhythm. Do not copy its character, outfit, colours or background. Character identity and facing remain owned by reference 1. For locomotion, use both left-forward and right-forward contacts; do not repeat one leg phase throughout the row."] : []),
        `Action: ${state.action}. ${state.loop ? "Make a cyclic action; the final pose must transition naturally to the first." : "Play the action once in the given order."}`,
        ...state.poses.map((pose, index) => `Cell ${index + 1}: ${pose}`),
        `Use a perfectly flat ${request.chroma} background, including gaps between limbs. No shadows, gradients or checkerboard.`,
        "Keep fixed camera, character scale, horizontal anchor and ground baseline across cells. Preserve intentional vertical motion. Each pose must fit fully inside its own cell with clear background on every edge. Do not overlap adjacent cells.",
    ].join("\n");
}
function json(value: unknown) { return Buffer.from(JSON.stringify(value, null, 2) + "\n"); }

function preview(manifest: { warnings: string[]; animations: { name: string; loop: boolean; frames: { x: number; y: number; width: number; height: number; durationMs: number }[] }[] }) {
    const data = JSON.stringify(manifest).replaceAll("<", "\\u003c");
    return `<!doctype html><html lang="en"><meta charset="utf-8"><title>Sprite animation comparison</title>
<style>body{background:#17191e;color:#eee;font:16px system-ui;padding:32px}canvas{image-rendering:pixelated;background:repeating-conic-gradient(#333 0% 25%,#444 0% 50%) 0/20px 20px}section{margin:24px 0}figure{display:inline-block;margin:0 24px 0 0}figcaption{margin:8px 0}button{margin:8px}a{color:#9bcaff}li{margin:6px 0}</style>
<h1>Sprite animation comparison</h1><p>Before: background removal only. After: selected cleanup and alignment. Both use the same scale and playback order.</p>
<p>Review limb alternation, character details and the loop seam. Automated metrics cannot verify gait. <a href="quality.json">Quality report</a></p><ul id="warnings"></ul><main></main>
<script>
const data=${data};
for(const warning of data.warnings){const item=document.createElement('li');item.textContent=warning;document.querySelector('#warnings').append(item)}
Promise.all(['before-atlas.png','atlas.png'].map(src=>new Promise((resolve,reject)=>{const image=new Image();image.onload=()=>resolve(image);image.onerror=reject;image.src=src}))).then(images=>{
for(const animation of data.animations){
const section=document.createElement('section'),title=document.createElement('h2');title.textContent=animation.name;section.append(title);
const first=animation.frames[0];
const contexts=images.map((image,i)=>{const figure=document.createElement('figure'),caption=document.createElement('figcaption'),canvas=document.createElement('canvas');caption.textContent=i?'After':'Before';canvas.width=first.width;canvas.height=first.height;canvas.style.width=Math.max(256,first.width)+'px';figure.append(caption,canvas);section.append(figure);return canvas.getContext('2d')});
let index=0,playing=true,last=0;const button=document.createElement('button');button.textContent='Pause';button.onclick=()=>{if(!playing&&index===animation.frames.length-1)index=0;playing=!playing;button.textContent=playing?'Pause':'Play';last=0};section.append(button);document.querySelector('main').append(section);
function tick(time){if(playing&&last&&time-last>=animation.frames[index].durationMs){if(index+1<animation.frames.length)index++;else if(animation.loop)index=0;else{playing=false;button.textContent='Replay'}last=time}if(!last)last=time;const frame=animation.frames[index];contexts.forEach((ctx,i)=>{ctx.clearRect(0,0,frame.width,frame.height);ctx.drawImage(images[i],frame.x,frame.y,frame.width,frame.height,0,0,frame.width,frame.height)});requestAnimationFrame(tick)}requestAnimationFrame(tick)
}}).catch(()=>{document.querySelector('main').textContent='Could not load comparison atlases. Keep this HTML beside both PNG files.'});
</script></html>`;
}
