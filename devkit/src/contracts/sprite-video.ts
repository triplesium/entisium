import { z } from "zod/v4";

export const spriteRun = z.string().regex(/^assets\/(?:[a-zA-Z0-9_-]+\/)*[a-zA-Z0-9_-]+$/);
export const spriteVideoOptions = z.object({
    cellSize: z.number().int().min(32).max(512).default(256),
    frameCount: z.number().int().min(4).max(32).default(16),
    align: z.boolean().default(true),
}).strict();
export const spriteVideoJobSchema = z.object({
    characterImage: z.string().regex(/^assets\/(?:[a-zA-Z0-9_-]+\/)*[a-zA-Z0-9_-]+\.png$/),
    action: z.string().trim().min(1).max(4000),
    name: z.string().regex(/^[a-zA-Z0-9_-]{1,48}$/).default("animation"),
    model: z.literal("minimax/h3-max-turbo/image-to-video").default("minimax/h3-max-turbo/image-to-video"),
    seed: z.number().int().min(0).max(2147483647).default(20260911),
    ...spriteVideoOptions.shape,
}).strict();
const waitMs = z.number().int().min(0).max(30000).default(20000);
export const spriteVideoCreateSchema = z.object({
    operation: z.literal("create"), run: spriteRun, ...spriteVideoJobSchema.shape, waitMs,
}).strict();
export const spriteVideoStatusSchema = z.object({ operation: z.literal("status"), run: spriteRun, waitMs }).strict();
export const spriteVideoReprocessSchema = z.object({
    operation: z.literal("reprocess"), run: spriteRun,
    video: z.string().regex(/^assets\/(?:[a-zA-Z0-9_-]+\/)*[a-zA-Z0-9_-]+\.mp4$/).optional(),
    name: z.string().regex(/^[a-zA-Z0-9_-]{1,48}$/).default("animation"),
    ...spriteVideoOptions.shape,
}).strict();
export type SpriteVideoJob = z.infer<typeof spriteVideoJobSchema>;
export type SpriteVideoOptions = z.infer<typeof spriteVideoOptions>;
export type SpriteVideoInput = z.infer<typeof spriteVideoCreateSchema> | z.infer<typeof spriteVideoStatusSchema> | z.infer<typeof spriteVideoReprocessSchema>;
