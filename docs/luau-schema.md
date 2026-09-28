# Data schemas

[`schema.luau`](../engine/scripting/libraries/schema/schema.luau) is a pure Luau module with no AI, ECS, context or host-service
dependencies. The standalone Luau host exposes it as `require("@schema")`.
Other hosts can resolve that alias to this file; game-runtime module resolution
does not yet automatically expose it. Generated SDK definitions include the alias
and editor types.

```lua
local schema = require("@schema")

local Decision = schema.object {
    action = schema.enum { "attack", "retreat", "collect" }:describe("Next action"),
    target = schema.optional(schema.integer()):describe("Target entity ID"),
    reason = schema.string(),
    waypoints = schema.array(schema.object { x = schema.number(), y = schema.number() }),
}

local decision = Decision:parse {
    action = "collect",
    reason = "Nearby pickup",
    waypoints = { { x = 2, y = 3 } },
}
local result = Decision:safe_parse({ action = "invalid" })
if not result.success then
    print(result.issues[1].message)
end
```

## API

| API | Contract |
| --- | --- |
| `string()` / `boolean()` | Exact primitive type, no coercion |
| `number()` | Finite number; rejects NaN and infinity |
| `integer()` | Finite integral number; ordinary Luau numeric precision |
| `enum { "a", "b" }` | Nonempty list of unique strings |
| `object { name = schema, ... }` | Required fields by default; rejects unknown and non-string keys |
| `array(element)` | Dense 1-based array, including an empty table |
| `optional(inner)` | Accept nil, including absent object fields |
| `spec:describe(text)` | New schema carrying a description; does not mutate the original |
| `spec:parse(value)` | Validate and return data, or throw a field-path error |
| `spec:safe_parse(value)` | `{ success = true, data = ... }` or `{ success = false, issues = ... }` |
| `spec:definition()` | Frozen plain description tree for consumers and future adapters |

Schemas copy their field/enum inputs and are immutable. Successful parsing makes
independent, recursively frozen tables. It does not mutate input, coerce values,
insert defaults, or silently strip fields. Optional nil object fields are absent
from the result. Optional array elements do not permit holes. Empty tables are
interpreted using the schema (object or array).

Validation accepts only plain tables, rejects metatables and cycles, and stops at
the first error. Issues contain a `path` array of field names and 1-based indices,
plus a message without the rejected value. For example, a nested error can be
reported as `$["waypoints"][1]["x"]: Expected finite number`. Limits are 64 levels
and 10,000 visited schema/value nodes; the host's execution budget also applies.

`definition()` preserves `kind` and optional `description`. Objects contain
`fields`, arrays contain `element`, enums contain `values`, and optional nodes
contain `inner`. This is the module's own description format, **not JSON Schema**.
Export is also bounded by depth and node count. Provider-specific JSON Schema
conversion belongs in an adapter; no network or serialization is performed here.

Luau's new solver infers object fields, nested arrays, optional values and
`safe_parse` result narrowing from the generated declarations. Enums currently
produce static `string` types, while enforcing exact members at runtime. Integer
schemas produce static `number`. The `__schema_value` field in declarations is
type-only metadata; it is not a runtime property.

Schema validation runs in Luau. The AI library adapts schema definitions to
provider-specific JSON Schema and handles optional/null values at its protocol
boundary; Node does not maintain a second schema implementation.
This version does not implement unions, null sentinels, recursive schemas,
transforms or reflected engine-type imports. Existing context descriptions remain unchanged.

Runnable example: `samples/projects/playtest_basics/assets/tests/schema.luau`.
Run it with `npm run playtest -- <project.yaml> <test.luau>`. It needs no model
credentials and starts no game session.
