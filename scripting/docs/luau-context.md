# Context views

For independent data validation, see [`@schema`](luau-schema.md).
For shared asynchronous execution and host adapters, see [`@task`](luau-task.md).

## Source types and SDK publication

Library implementations use `--!strict` and export their own types. The SDK
generator copies these sources unchanged, alongside the native C++ declarations.
`local task = require("@task")` exposes `task.Task<number>`;
`local schema = require("@schema")` exposes `schema.Schema<T>`.

The LSP tests check the library source modules and import them in type
inference tests. The generator tests verify that published modules match their
source bytes. After changing a library, run
`xmake test -y entisium-lsp-tests/default entisium-luau-defgen-tests/default`
and the DevKit Luau host regression tests.

## Context graphs

[`context.luau`](../libraries/context/context.luau) implements immutable observation graphs without depending on a
World, Playtest or a model SDK. The Playtest adapter exposes it as `@context`.
`@context/core` is embedded in every Luau VM; it does not read library sources
from the filesystem. Other hosts can use `bind(adapter)` to supply their own reader.

See `samples/projects/playtest_basics/assets/tests/context.luau` for a runnable
example. Run it with the same `npm run playtest -- <project> <entry>` command as
other tests. No LLM service is required.

## API

| API | Meaning |
| --- | --- |
| `resource(Type)` | Required resource source |
| `resource(Type):optional()` | Missing resource produces nil; other errors still fail |
| `query { name = Type, ... }` | Entity rows with named components; `entity` is reserved |
| `value(value)` | Copy a fixed value when defining the source |
| `combine { name = source, ... }` | Combine sources into a nested object |
| `source:map(fn)` | Transform the entire source value |
| `source:describe(text)` | Attach output description; returns a new node |
| `query:filter(fn)` | Keep matching rows |
| `query:order_by(fn, "asc" or "desc")` | Stable sorting by number or string, ascending by default |
| `query:take(n)` | Keep at most n rows |
| `query:map(fn)` | Transform each row; returning nil is an error |
| `query:count()` | Scalar count after preceding operations |
| `query:first()` | First row, or nil |
| `view { description = text, fields = sources }` | Define a reusable view |
| `view:sample()` | Read one immutable sample without advancing game time |
| `sample:encode()` | JSON containing data, schema, frame and simulation time |
| `array(values)` | Mark an array, including an empty array, for JSON encoding |

Sample fields are `data`, `schema`, `frame` and `simulation_time`. The encoder runs
at sampling time, so encoding errors fail the sample immediately. `encode()`
returns that saved representation. Descriptions are stored beside the matching
field under `schema.fields`; nested `combine` nodes retain nested descriptions.
`map` changes meaning, so attach its output description after the projection.
Optional missing object fields are omitted from JSON, with `optional: true` in
their schema. Empty query results encode as `[]`; an unmarked empty table encodes
as `{}`. Use `array {}` when a projection creates an empty array itself.

## Sampling and purity

The graph collects resource and component requests in stable field-name order.
Equivalent reads are merged, and all reads execute in one native inspection
request at one simulation boundary. Required and optional resource reads are
distinct requests. Each graph node is evaluated once per sample. Views hold no
history; samples hold independent frozen copies and owned read-only reflected
values. Sharing a source across views does not share mutable samples.

Callbacks run in the test VM on these snapshots. They must be pure: no ECS writes,
new reads, waits or model calls. A callback that yields is rejected before the
host operation is dispatched. Lua cannot prevent arbitrary mutation of captured
variables, so avoiding those side effects remains the callback author's contract.
Errors identify the context field path. Sorting is stable and operations execute
in the written order. `take` and `filter` currently do not reduce native snapshot
transfer size. Native batches allow 1–32 read requests and at most 1 MiB combined
output; component queries retain their 4096-entity bound.

## Types and encoding

The `@context` source exports types using Luau's new solver and type functions to
preserve named component fields and nested sample shapes. Annotate callback
parameters and return types, especially `map`: current Luau inference does not
reliably infer these through recursive fluent methods. The typed example shows
this convention. Once annotated, projected types propagate into `sample.data`
and incompatible field assignments are diagnosed.

The runtime encoder accepts finite numbers, strings, booleans, dense arrays,
string-keyed objects and supported reflected values. Reflected values use the
engine serializer, including its enum representation and nested field behavior.
Functions, unsupported userdata, sparse/mixed tables and cycles fail explicitly.
Encoding is bounded by depth, node count and final byte size. This is not a
lossless arbitrary-precision integer transport: ordinary Luau and Node numbers
have JavaScript/double precision limits. Do not use large integers as opaque IDs.

## Adapter contract

`bind` receives these functions:

- `request(source)` converts a resource/query node into a transport request.
- `sample(requests)` returns `{ batch, frame, simulation_time }` in request order.
- `decode(source, response)` returns a resource value or named entity rows.
- `encode(value)` creates JSON, including supported reflected values.
- `copy_value(value)` makes an owned read-only copy of reflected userdata.

The Playtest adapter lives in `scripting/libraries/playtest/context.luau`. Node forwards
the batch to the existing `test.snapshot` provider. Existing observe and segment
interfaces remain available. Model integration, history sessions and engine-side
spatial filtering are outside this version.
