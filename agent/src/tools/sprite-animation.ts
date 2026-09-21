import type { AgentTool } from "@earendil-works/pi-agent-core";
import type { TSchema } from "typebox";
import { z } from "zod/v4";
import { spriteAnimationSchema, type SpriteAnimationInvoker } from "@entisium/devkit/contracts/sprite-animation";

export function createSpriteAnimationTools(invoke: SpriteAnimationInvoker): AgentTool<any>[] {
    return [{
        name: "sprite_animation", label: "Sprite animation",
        description: "Default: operation=create takes run, characterImage and action, generates ONE H3 480p/5-second video, then automatically exports soft-matted aligned looping sprites, atlas, animation JSON and HTML preview. Use a flat #00ff00 background with no green character materials and full-body margins. V1 targets smooth non-pixel cartoon loops. Optional name/seed/cellSize/frameCount/align. waitMs defaults to 20000 (max 30000); pending returns jobId equal to run. Continue operation=status with the SAME run; never create another run just to poll. Uncertain submission must be checked with the provider, not resubmitted. operation=reprocess uses the saved video or an explicit assets/...mp4 without paid calls; supports cellSize/frameCount/align and writes a fresh export. Play durationMs, not average fps. Requires ffmpeg and ffprobe on the host. Legacy image-row workflow remains available in stages: prepare creates a NEW run directory from an existing character PNG and explicit per-state poses, preserving reference and layout guides. Optional states[].motionReference supplies a separate multi-pose gait PNG with both opposite foot contacts; it is preserved and sent as motion-only guidance. generate makes ONE paid image call for ONE state using those references. Never automatically retry a failed/cancelled generate; its attempt may have incurred a charge. compose is local: remove edge-connected chroma background, extract equal-width cells, despill edges and align upper bodies horizontally (despill/align default true; disable for comparisons or intentional sideways motion), apply optional frame order/FPS/loop selections, and export a new PNG atlas, individual PNG frames, JSON pixel rectangles, quality.json and a synchronized before/after HTML preview. Quality metrics flag low motion, near-duplicates, drift and possible colour spill; they never certify gait. All states need raw images before compose; selections.source can import an existing row. Original files are preserved. Empty/clipped frames fail validation. Review animation visually; extraction cannot verify anatomy or motion quality. Use explicit frame rectangles from animation.json for runtime playback, never infer a grid. Start with one short action; use a new run for additional generation takes.",
        parameters: z.toJSONSchema(spriteAnimationSchema) as TSchema,
        execute: async (_id, parameters, signal) => {
            signal?.throwIfAborted();
            const result = await invoke(spriteAnimationSchema.parse(parameters), signal);
            return { content: [{ type: "text" as const, text: JSON.stringify(result) }], details: result };
        },
    }];
}
