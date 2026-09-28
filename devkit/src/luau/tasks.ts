import { z } from "zod/v4";

export type TaskDispatch = (method: string, payload: unknown, signal: AbortSignal) => Promise<unknown>;
type State = { status: "pending" } | { status: "completed"; value: unknown; settled_ms: number } | { status: "failed"; error: string; settled_ms: number };

/** Session-scoped background operations, independent of any particular service. */
export class HostTasks {
    private entries = new Map<string, { controller: AbortController; state: State }>();
    private nextId = 0;
    private closed = false;
    constructor(private readonly dispatch?: TaskDispatch) {}

    async handle(method: string, payload: unknown): Promise<unknown> {
        if (method === "task.clock") return { now_ms: performance.now() };
        if (method === "task.idle") {
            await new Promise(resolve => setTimeout(resolve, 25));
            return { now_ms: performance.now() };
        }
        if (method === "task.submit") {
            if (this.closed || !this.dispatch) throw new Error("Background services are unavailable.");
            if (this.entries.size >= 2048) throw new Error("Task session limit exceeded.");
            const request = z.object({ method: z.string().min(1), payload: z.unknown() }).strict().parse(payload);
            const id = String(++this.nextId);
            const entry = { controller: new AbortController(), state: { status: "pending" } as State };
            this.entries.set(id, entry);
            void Promise.resolve().then(() => {
                entry.controller.signal.throwIfAborted();
                return this.dispatch!(request.method, request.payload, entry.controller.signal);
            }).then(value => {
                if (entry.state.status === "pending") entry.state = { status: "completed", value: value ?? {}, settled_ms: performance.now() };
            }, error => {
                if (entry.state.status === "pending") entry.state = { status: "failed", error: error instanceof Error ? error.message : String(error), settled_ms: performance.now() };
            });
            return { id };
        }
        if (method === "task.cancel") {
            const { id } = z.object({ id: z.string() }).strict().parse(payload);
            const entry = this.get(id);
            if (entry.state.status === "pending") {
                entry.state = { status: "failed", error: "Task cancelled.", settled_ms: performance.now() };
                entry.controller.abort();
            }
            return {};
        }
        if (method === "task.poll" || method === "task.wait") {
            const { ids } = z.object({ ids: z.array(z.string()).min(1).max(2048) }).strict().parse(payload);
            const entries = ids.map(id => this.get(id));
            // A bounded wait releases the protocol frequently for cancellation.
            if (method === "task.wait" && entries.some(entry => entry.state.status === "pending")) {
                await new Promise(resolve => setTimeout(resolve, 25));
            }
            return { now_ms: performance.now(), states: Object.fromEntries(ids.map(id => [id, this.get(id).state])) };
        }
        throw new Error(`Unknown task operation: ${method}`);
    }
    private get(id: string) {
        const entry = this.entries.get(id);
        if (!entry) throw new Error("Unknown task handle.");
        return entry;
    }
    close() {
        this.closed = true;
        for (const entry of this.entries.values()) {
            if (entry.state.status === "pending") {
                entry.state = { status: "failed", error: "Task session closed.", settled_ms: performance.now() };
                entry.controller.abort();
            }
        }
    }
}
