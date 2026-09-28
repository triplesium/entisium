-- Hand-authored declarations for the runtime API that is not represented by
-- C++ reflection metadata. This file is merged into generated definitions.

export type entity = number
export type Entity = entity
export type i8 = number
export type i16 = number
export type i32 = number
export type i64 = number
export type u8 = number
export type u16 = number
export type u32 = number
export type u64 = number
export type f32 = number
export type f64 = number

-- Reflected containers provide indexing, length and generalized iteration.
-- They are userdata at runtime; table library functions and index assignment
-- are not supported, even when structural typing accepts them.
export type ReflectedSequence<T> = typeof(setmetatable(
    {} :: {[number]: T},
    {} :: {
        __len: (any) -> number,
        __iter: (any) -> (() -> (number, T)),
    }
))
export type ReflectedMap<K, V> = typeof(setmetatable(
    {} :: {[K]: V},
    {} :: {
        __len: (any) -> number,
        __iter: (any) -> (() -> (K, V)),
    }
))

-- Reflected type values carry this phantom field in the generated declarations.
-- It lets generic runtime methods infer T from a value such as Transform2d.
export type TypeToken<T> = {
    __ets_type: T?,
}

-- Native bridge functions consumed by the typed Playtest adapters.

-- Asset handles retain the asset type selected by a TypeToken<T> argument.
export type Handle<T> = {
    __ets_asset_type: T?,
}

-- System parameter wrappers expose the wrapped value to Luau's type checker.
export type Read<T> = T
export type Write<T> = T
export type ResRO<T> = T
export type ResRW<T> = T
export type Local<T> = T

-- Inline query filters are conventionally trailing. Nil placeholders preserve
-- the types and positions of all preceding fields in the iterator result.
export type With<T> = nil
export type Without<T> = nil
export type Added<T> = nil
export type Changed<T> = nil
export type Or<T...> = nil

export type Query<
    T1,
    T2 = nil,
    T3 = nil,
    T4 = nil,
    T5 = nil,
    T6 = nil,
    T7 = nil,
    T8 = nil,
    T9 = nil,
    T10 = nil,
    T11 = nil,
    T12 = nil,
    T13 = nil,
    T14 = nil,
    T15 = nil,
    T16 = nil
> = typeof(setmetatable(
    {} :: {
        size: (self: Query<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16>) -> number,
        empty: (self: Query<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16>) -> boolean,
        first: (self: Query<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16>) -> (T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16),
    },
    {} :: {
        __iter: (self: any) -> (() -> (T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16)),
    }
))
export type Filtered<T, F...> = T

export type EventReader<T> = typeof(setmetatable(
    {} :: {
        next: (self: EventReader<T>) -> T?,
        reset: (self: EventReader<T>) -> (),
    },
    {} :: {
        __iter: (self: EventReader<T>) -> (() -> T),
    }
))
export type EventReaderRO<T> = typeof(setmetatable(
    {} :: {
        next: (self: EventReaderRO<T>) -> T?,
        reset: (self: EventReaderRO<T>) -> (),
    },
    {} :: {
        __iter: (self: EventReaderRO<T>) -> (() -> T),
    }
))
export type EventWriter<T> = typeof(setmetatable(
    {} :: {
        send: (self: EventWriter<T>, event: T) -> (),
    },
    {} :: {
        __call: (self: EventWriter<T>, event: T) -> (),
    }
))

export type State<T> = {
    get: (self: State<T>) -> T,
}
export type NextState<T> = {
    set: (self: NextState<T>, value: T) -> NextState<T>,
    clear: (self: NextState<T>) -> NextState<T>,
    reset: (self: NextState<T>) -> NextState<T>,
}
export type RemovedComponents<T> = typeof(setmetatable(
    {} :: {
        next: (self: RemovedComponents<T>) -> Entity?,
        removed: (self: RemovedComponents<T>) -> Entity?,
        clear: (self: RemovedComponents<T>) -> (),
    },
    {} :: {
        __iter: (self: RemovedComponents<T>) -> (() -> Entity),
    }
))

export type EntityCommands = {
    add: (self: EntityCommands, ...any) -> EntityCommands,
    remove: (self: EntityCommands, ...any) -> EntityCommands,
    has: (self: EntityCommands, type_token: any) -> boolean,
    set_parent: (self: EntityCommands, parent: Entity) -> EntityCommands,
    remove_parent: (self: EntityCommands) -> EntityCommands,
    despawn: (self: EntityCommands) -> (),
    id: (self: EntityCommands) -> Entity,
}
export type Commands = {
    spawn: (self: Commands, ...any) -> EntityCommands,
    entity: (self: Commands, entity: Entity) -> EntityCommands,
    add_resource: (self: Commands, ...any) -> Commands,
}

export type WorldEntity = {
    id: (self: WorldEntity) -> Entity,
    has: <T>(self: WorldEntity, type_token: TypeToken<T>) -> boolean,
    get: <T>(self: WorldEntity, type_token: TypeToken<T>) -> T?,
    add: (self: WorldEntity, ...any) -> WorldEntity,
    remove: (self: WorldEntity, ...any) -> WorldEntity,
    despawn: (self: WorldEntity) -> (),
    parent: (self: WorldEntity) -> Entity?,
    children: (self: WorldEntity) -> { Entity },
    set_parent: (self: WorldEntity, parent: Entity) -> WorldEntity,
    remove_parent: (self: WorldEntity) -> WorldEntity,
}
export type World = {
    has_entity: (self: World, entity: Entity) -> boolean,
    entity: (self: World, entity: Entity) -> WorldEntity?,
    spawn: (self: World, ...any) -> WorldEntity,
    has_resource: <T>(self: World, type_token: TypeToken<T>) -> boolean,
    resource: <T>(self: World, type_token: TypeToken<T>) -> T?,
    set_resource: <T>(self: World, value: T) -> T,
    query: (self: World, descriptors: { any }) -> Query<any>,
    commands: (self: World) -> Commands,
}

export type Schedule = number | string
export type SystemFunction = (...any) -> ()
export type RunCondition = (...any) -> boolean
export type StateCondition = {
    value: any,
}
export type SystemConfig = {
    before: (self: SystemConfig, first: SystemFunction, ...SystemFunction) -> SystemConfig,
    after: (self: SystemConfig, first: SystemFunction, ...SystemFunction) -> SystemConfig,
    run_if: (
        self: SystemConfig,
        first: RunCondition | StateCondition,
        ...(RunCondition | StateCondition)
    ) -> SystemConfig,
}
export type System = SystemFunction | SystemConfig
export type SystemChain = {
    __ets_system_chain: boolean?,
}
export type SystemGroup = System | SystemChain

export type App = {
    init_state: <T>(self: App, initial_value: T) -> (),
    insert_state: <T>(self: App, initial_value: T) -> (),
    add_event: <T>(self: App, event_type: TypeToken<T>) -> (),
    add_resource: <T>(self: App, value: T) -> (),
    insert_resource: <T>(self: App, value: T) -> (),
    add_system: (self: App, schedule: Schedule, system: SystemGroup) -> (),
    add_systems: (self: App, schedule: Schedule, first: SystemGroup, ...SystemGroup) -> (),
    add_playtest: (self: App, playtest: any) -> (),
}

declare Plugin: {
    new: (definition: { build: (app: App) -> () }) -> any,
}

declare function system(callback: System): SystemConfig
declare function chain(first: SystemGroup, second: SystemGroup, ...: SystemGroup): SystemChain
declare function in_state<T>(value: T): StateCondition
declare function OnEnter<T>(value: T): Schedule
declare function OnExit<T>(value: T): Schedule
declare function OnTransition<T>(from: T, to: T): Schedule

declare function Read<T>(type_token: TypeToken<T>): any
declare function Write<T>(type_token: TypeToken<T>): any
declare function With<T>(type_token: TypeToken<T>): any
declare function Without<T>(type_token: TypeToken<T>): any
declare function Added<T>(type_token: TypeToken<T>): any
declare function Changed<T>(type_token: TypeToken<T>): any
declare function Or(first: any, ...: any): any
declare field: ((type_token: any) -> any) & ((type_token: any, default_value: any) -> any)
declare function optional(type_token: any): any

declare Entity: {
    kind: string,
}

declare First: Schedule
declare PreStartUp: Schedule
declare StartUp: Schedule
declare PreUpdate: Schedule
declare Update: Schedule
declare PostUpdate: Schedule
declare Last: Schedule
declare RenderPrepare: Schedule
declare RenderFirst: Schedule
declare RenderStart: Schedule
declare RenderUpdate: Schedule
declare RenderEnd: Schedule
declare RenderLast: Schedule
declare RunFixedMainLoop: Schedule
declare FixedFirst: Schedule
declare FixedPreUpdate: Schedule
declare FixedUpdate: Schedule
declare FixedPostUpdate: Schedule
declare FixedLast: Schedule

declare MainSchedules: {
    First: Schedule,
    PreStartUp: Schedule,
    StartUp: Schedule,
    PreUpdate: Schedule,
    Update: Schedule,
    PostUpdate: Schedule,
    Last: Schedule,
    RenderPrepare: Schedule,
    RenderFirst: Schedule,
    RenderStart: Schedule,
    RenderUpdate: Schedule,
    RenderEnd: Schedule,
    RenderLast: Schedule,
    RunFixedMainLoop: Schedule,
    FixedFirst: Schedule,
    FixedPreUpdate: Schedule,
    FixedUpdate: Schedule,
    FixedPostUpdate: Schedule,
    FixedLast: Schedule,
}
