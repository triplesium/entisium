# Luau JSON

`@json` is a built-in library shared by game and Playtest VMs. Its public API and
Luau types live in `engine/scripting/libraries/json/json.luau`; C++ implements encoding and decoding
with the existing nlohmann JSON dependency. It needs neither Node nor a task session.
The generated SDK copies the same Luau source and exposes the `@json` alias.

```luau
local json = require("@json")

local body = json.encode({
    message = "hello",
    items = json.array({}),
    missing = json.null,
})
local value = json.decode(body) -- unknown; validate before using fields
```

## API

| API | Result |
| --- | --- |
| `json.encode(value: unknown)` | A JSON string; raises on unsupported values |
| `json.decode(text: string)` | `unknown`; raises on malformed or oversized JSON |
| `json.array<T>(values: {T})` | A shallow copy tagged as an array, including when empty |
| `json.null` | A VM-local immutable sentinel of type `json.Null` |

`decode` deliberately has no generic type parameter. Validate decoded data with
`@schema` or narrow `unknown` explicitly; a type assertion does not validate it.

```luau
local schema = require("@schema")
local result = schema.object({ name = schema.string() })
local data = result:parse(json.decode('{"name":"player"}'))
local name: string = data.name
```

## Tables and null

- An ordinary empty table encodes as `{}`; `json.array({})` encodes as `[]`.
- A nonempty table with consecutive integer keys starting at 1 encodes as an array.
- String-keyed tables encode as objects. Mixed keys, gaps, nonpositive indices,
  other key types, and tables with metatables are rejected.
- Decoded arrays retain their identity even after every element is removed.
  Array identity lives in a VM weak-key registry, not a table metatable.
- `json.array` copies only the container; it does not recursively validate elements.
  Its input must be a plain dense array.
- JSON `null` decodes to `json.null`, never `nil`. It therefore preserves object
  fields and array positions. Encoding `nil` is an error.
- Array tags are not automatically retained by `table.clone`, schema parsing, or
  other table-copying operations. Use `json.array` when encoding a new empty array.

`@schema` optional values still use `nil` for absence. JSON null is not automatically
converted to an absent field; a protocol adapter must decide whether that conversion
is appropriate. Decoded tables have no metatables and work with schema validation.

## Limits and errors

Text is UTF-8 JSON; invalid UTF-8, NaN, infinity, unsupported values such as functions,
threads or ordinary userdata, and cycles raise errors. Shared noncyclic tables are
allowed. Encoding reports the offending field path where applicable.

Both directions enforce an 8 MiB text limit, a nesting-depth limit of 64, and a
100,000-value limit. Encoding also bounds aggregate input string/key bytes before
serialization. Numbers use Luau's double precision; integers beyond its exact range
are not guaranteed to round-trip exactly. Use strings for large numeric identifiers.

Catch errors with `pcall` when invalid external input is expected. Encoding and
decoding are synchronous and do not yield.

## HTTP

Inside a task session:

```luau
local http = require("@http")
local json = require("@json")
local response = http.request({
    url = "https://example.com/api",
    method = "POST",
    headers = {{name = "Content-Type", value = "application/json"}},
    body = json.encode({items = json.array({})}),
}):await()
assert(response.status >= 200 and response.status < 300)
local value = json.decode(response.body)
```

HTTP continues to handle raw bytes. JSON encoding and response validation are explicit.
