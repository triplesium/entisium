# Luau libraries

Each library is an independent xmake target. File lists and target dependencies
are the build contract; C++ annotations describe native exports. No handwritten
JSON, header/source path settings, or per-file export rules are needed.

Libraries live in [`scripting/libraries`](../libraries/), with Luau sources,
native adapters and tests together. The VM, compiler and common binding
interfaces remain in [`engine/scripting`](../../engine/scripting/).
See the [Scripting index](../README.md) for public modules and examples.

## Native library

```lua
target("entisium-luau-example")
    set_kind("static")
    add_rules("entisium.luau_library")
    add_files("example.cpp")
    add_headerfiles("example.hpp")
```

```cpp
#include "refl/reflect.hpp"

ETS_REFLECT(LuauLibrary(name = "@example"))
struct Example {
    static int add(int a, int b);

    ETS_REFLECT(LuauExport(name = "enabled"))
    static bool is_enabled();
};
```

The rule scans the target's header files through reflgen. Unmarked classes are
ignored; exactly one marked library is allowed per target. The module name comes
from the marker. Binding code and native Luau types come from the same signatures.
Existing engine headers can use the reflection macros without depending on the
scripting runtime. The binding target depends on the engine implementation.

Public ordinary methods are exported. Constructors, fields and inherited methods
are not. Instance methods use one instance per VM, constructed with LuauLibraryServices&
when available, or with a default constructor. Static-only classes need not be constructible. Supported automatic conversions are booleans,
checked numbers, strings/string views, reflected objects and containers through
the shared reflection bridge. Raw pointers require an explicit adapter. See
Reflected native methods below for ownership and table conversion rules.

## Pure Luau library

```lua
target("entisium-luau-schema")
    set_kind("static")
    add_rules("entisium.luau_library")
    add_files("schema.luau")
```

Pure Luau modules use the target name after the entisium-luau- prefix, yielding
@schema here. Other target names are used verbatim after @. Set luau.name only
when a different name is needed, for example @context/core.

Source files must return tables:
- One .luau file is automatically the entry.
- Multiple .luau files require a unique init.luau entry; the other files must
  be inside its directory.
- Relative require("./helper") and require("../shared") resolve registered
  files only, with .luau and /init.luau suffix lookup.
- Internal sources are cached per VM, reject import cycles and cannot escape
  the library directory or be imported directly by project scripts.
- No source file means the native exports table is the public module.

## Mixed or custom library

```lua
target("entisium-luau-http")
    set_kind("static")
    add_rules("entisium.luau_library")
    add_deps("entisium-http", "entisium-luau-task")
    add_files("http.cpp", "http.luau")
    add_headerfiles("http.hpp")
```

A custom native adapter, such as JSON, uses LuauLibrary(name = "@json", custom = true) and
implements LuauNativeLibrary. Its constructor takes LuauLibraryServices&;
open pushes exactly one exports table under a protected Lua call. Its
LuauLibrary annotation supplies globals (global native type declarations),
types (module-local type aliases) and exports (the
exports table's type members). These describe custom Lua values and functions;
they do not create runtime exports. JSON uses this for its null sentinel and
generic array marker. The null identity declaration belongs to the JSON header;
reflgen stores it in the library manifest and defgen includes it in globals.d.luau
only when that library is in the host catalog. It is not part of the common
runtime declarations.

Automatic bindings currently reject enum types in parameters, return values and
reflected fields, including interfaces with signature overrides. Enum support
requires both type declarations and runtime value exports.

Automatic bindings generate their contract from C++ signatures. Only type
information erased from those signatures needs a local supplement:

```cpp
namespace ets::luau::ai {
ETS_REFLECT()
struct Model { /* reflected fields */ };
}

ETS_REFLECT(LuauExport(signature = "<T>(TypeToken<T>, string) -> T"))
static const Val decode_snapshot(TypeId type, std::string text);
```

Each native library uses its own ets::luau::<library> namespace. Its entry
class is Library, while payloads use names such as Model, Connection, Request
and Response directly. LuauType.name is available for exceptional aliasing,
but these libraries do not need it. LuauType.input replaces only the
corresponding Input alias, preserving generated result fields and methods. HTTP
uses it to express the url versus connection/path request alternatives. Ordinary
quoted C++ strings, including adjacent literals and escaped quotes, are supported.
Signature/type supplements describe the script contract; they do not change C++
argument conversion or runtime validation. Test them against the generated SDK.

Opaque classes with no reflected fields receive a branded type and no table
initializer alias. ets::luau::http::Connection is such a concrete C++ value; its private
shared identity keeps copied handles equal and prevents identity reuse while a
handle survives. HTTP validates handles against its own live connection registry,
including the closed state. The library API no longer exposes an erased Ref.

AI, HTTP, Context and JSON no longer have handwritten native.d.luau files. An
explicit .d.luau file remains supported as a full contract override for other
libraries; custom declaration metadata cannot be combined with that override.

Mixed source calls its private native table through require("@internal/http").
Direct add_deps edges to Luau library targets become public module dependencies
automatically. Dependencies on ordinary C++ targets do not become script imports.
Only a library's own internal native module is made available by this convention.
Credentials and connection ownership remain inside the HTTP adapter.

## Core, library targets and host catalogs

entisium-scripting-core contains the VM, compiler, reflection bridge and common
library interfaces. Each library rule adds a dependency on this core. The core
does not depend on any Luau library. entisium-scripting is a compatibility bundle
of the usual JSON, Task, Schema, HTTP, AI and Context libraries.

A host can depend on that bundle or select individual library targets. The
project-wide catalog rule activates for binary/shared hosts and SDK targets that
depend on scripting-core. For executable hosts, it generates an explicit default_luau_libraries function
referencing each reachable library's unique registration function. Static
linkers therefore retain selected libraries without global constructor
registration. Duplicate module names and missing/cyclic dependencies are errors.

Each library generates its own C++ and manifest under its target's autogendir.
The host merges the manifests reachable through direct dependency edges, skipping
SDK targets and their subgraphs. SDK targets merge their own dependency graph,
including explicitly selected tooling libraries and private source files for type
resolution. Conflicting SDK file paths are errors; obsolete owned files are
removed. File lists, source contents, contracts, parsed includes, target
dependencies and generator changes invalidate generation. Unchanged outputs are
preserved. Generated JSON files are tool interchange files, not author inputs.

## Host SDK builds

Each host using `entisium.luau-definitions` has a separate `<host>-sdk` phony
target using `entisium.luau-sdk` and `set_values("luau.sdk.host", "<host>")`.
The host and SDK share one dependency list in their xmake file. SDK targets may
add tooling libraries, such as `entisium-luau-playtest`, without registering those
libraries in the game VM. Missing host dependencies in an SDK are errors.

```sh
xmake build -y entisium-runtime-host-sdk
xmake build -y entisium-luau-host-sdk
# In a WASM configuration:
xmake build -y entisium-editor-runtime-sdk
```

Building an SDK builds the required libraries and reflection metadata but does
not compile or link the host executable. Building the host also builds its SDK.
Outputs are isolated at `<host-targetdir>/luau-definitions/<host-name>/`.
Consumers read `entisium.luau-definitions.output` from the target. The native LSP
uses the `entisium-runtime-host` SDK by default; `--definitions-index` selects a
different SDK. The Editor accepts `ETS_ENTISIUM_LUAU_DEFINITIONS_INDEX`.

SDK generation tracks reflection manifests, the merged library catalog, manual
runtime declarations and generator changes. `sdk-files.json` lists all published
files; a missing output triggers regeneration. Unchanged inputs skip defgen,
and unchanged generated content preserves file timestamps. WASM LSP packaging
tracks these files as link inputs so changes refresh the embedded SDK.

Playtest modules are ordinary `entisium.luau_library` targets with explicit names
and dependencies in `scripting/libraries/playtest/xmake.lua`. Their sources are
embedded in the standalone host and published from its catalog; the host loader
and defgen contain no per-module source paths.

## Ownership

Exports are readonly tables with environments independent of project importers.
Failed loads release Lua references and can be retried. Native instances,
including failed openers, survive lua_close so userdata finalizers can still
access them. Call LuauLibraries::close before closing the VM and destroy the
registry afterwards. cancel cancels native operations, pending reports work and
diagnostic exposes optional library-specific diagnostics.

HTTP transport stays in engine/http. HTTP Task composition stays in Luau.
The Task clock, AI configuration lookup and Context bridge are separate native
interfaces. Context bridge requires host calls to be enabled.
HTTP tape diagnostics use library_diagnostic("@http", "tape").

## Verification

- entisium-luau-catalog-tests: selected target aggregation and relative sources.
- entisium-scripting-tests: binding conversion, lifetime, isolation and reflection.
- entisium-luau-libraries-tests: JSON, HTTP, AI and Context.
- entisium-reflgen-tests: header discovery, invalid contracts and embedded sources.
- entisium-luau-defgen-tests and entisium-lsp-tests: SDK output and type contracts.

## Reflected native methods

Automatic libraries use the existing MethodImpl, Ref/Val, ReturnAdapter and
Luau reflection bridge. They may have a default constructor or take
LuauLibraryServices&. They do not implement open or manipulate the Lua stack.
Reflected payloads use the existing ETS_REFLECT() marker and generated property
metadata; there is no separate library object or container codec system.

The table argument conversions below apply to native library bindings. Ordinary
engine methods and positional constructors do not advertise these conversions;
existing record initialization such as `Vector3.new({x = 1, y = 2, z = 3})` remains
available.

Arguments accept reflected userdata directly (without copying const-reference
arguments), or plain table initializers for reflected records, indexed containers
and maps. Omitted record fields keep their C++ defaults. Unknown fields, sparse
arrays, incompatible values and excessive nesting fail with field/index context.
Both ets::Optional and std::optional use the existing optional container adapter.
Numeric arguments use their declared type, with finite and exact integer checks.

Owned results remain reflected userdata. Instance methods returning a reference
with no arguments retain the library instance; raw pointer exports and reference
returns with arguments are rejected until an explicit lifetime policy exists.
Ref returns are an explicit low-level contract: their referent must be owned by
the library instance. Ref/Val parameters reuse the existing reflected value bridge.

Returned records and containers use reflected userdata. Sequence indexing is
1-based; missing indices and map keys return nil. Containers support #value and
for key, value in container do. Structural mutation and table-library functions
(such as table.insert or table.clone) are not supported. Child objects and
iterators retain their owner and preserve const access. Borrowed ECS values
still expire with their borrow scope.

There are no table/readonly export flags. Const value returns use the existing
readonly reflected-value path; Context returns const Val to preserve its
readonly snapshot contract. Result errors retain reflection's nil,error
convention; exceptions become Lua errors. ReflectedSequence/ReflectedMap SDK
aliases describe container access instead of declaring returned containers as
ordinary arrays. These are structural Luau aliases for compatibility with both
solvers; they cannot fully express userdata identity, so some table-library
calls can still type-check and will fail at runtime. Input tables continue to
initialize C++ records and containers.

AI, HTTP and Context now expose ordinary C++ methods. JSON remains a custom
adapter because its null sentinel and array marking are Lua-specific; its stack
operations live in libraries/json. Shared JSON/value conversion used by Context
and reflection stays in scripting/src/detail/json_value.cpp.
