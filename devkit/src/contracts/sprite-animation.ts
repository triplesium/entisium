import { z } from "zod/v4";
import { imageOptionsSchema } from "./image-generation.js";
import { spriteVideoCreateSchema, spriteVideoStatusSchema, spriteVideoReprocessSchema } from "./sprite-video.js";

const asset = z.string().regex(/^assets\/(?:[a-zA-Z0-9_-]+\/)*[a-zA-Z0-9_-]+\.png$/);
const run = z.string().regex(/^assets\/(?:[a-zA-Z0-9_-]+\/)*[a-zA-Z0-9_-]+$/);
const name = z.string().regex(/^[a-zA-Z0-9_-]{1,48}$/);
export const spriteStateSchema = z.object({
    name, action: z.string().trim().min(1).max(4000),
    motionReference: asset.optional(),
    poses: z.array(z.string().trim().min(1).max(400)).min(2).max(16),
    fps: z.number().min(1).max(60).default(8), loop: z.boolean().default(true),
}).strict();
export const spriteRequestSchema = z.object({
    reference: asset,
    description: z.string().trim().min(1).max(4000),
    cellSize: z.number().int().min(16).max(512).default(128),
    chroma: z.string().regex(/^#[0-9a-fA-F]{6}$/).default("#ff00ff"),
    states: z.array(spriteStateSchema).min(1).max(16),
}).strict().refine(value => new Set(value.states.map(state => state.name.toLowerCase())).size === value.states.length,
    "State names must be unique, ignoring case.");
const selection = z.object({
    state: name, source: asset.optional(),
    order: z.array(z.number().int().min(0).max(15)).min(1).max(32).optional(),
    fps: z.number().min(1).max(60).optional(), loop: z.boolean().optional(),
}).strict();
export const spriteAnimationSchema = z.discriminatedUnion("operation", [
    spriteVideoCreateSchema, spriteVideoStatusSchema, spriteVideoReprocessSchema,
    z.object({ operation: z.literal("prepare"), run, request: spriteRequestSchema }).strict(),
    z.object({ operation: z.literal("generate"), run, state: name, options: imageOptionsSchema.optional() }).strict(),
    z.object({ operation: z.literal("compose"), run,
        tolerance: z.number().int().min(0).max(120).default(40),
        despill: z.boolean().default(true),
        align: z.boolean().default(true),
        selections: z.array(selection).max(16).default([]),
    }).strict(),
]);
export type SpriteRequest = z.infer<typeof spriteRequestSchema>;
export type SpriteAnimationInput = z.infer<typeof spriteAnimationSchema>;
export type SpriteAnimationResult = { operation: SpriteAnimationInput["operation"]; run: string; paths: string[]; warnings?: string[];
    jobId?: string; status?: "pending" | "completed" | "failed" | "uncertain"; message?: string };
export type SpriteAnimationInvoker = (input: SpriteAnimationInput, signal?: AbortSignal) => Promise<SpriteAnimationResult>;
